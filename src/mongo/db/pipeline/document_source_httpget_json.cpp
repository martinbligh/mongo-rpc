/**
*    Copyright (C) 2011 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include <stdio.h>
#include "mongo/bson/json.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/util/log.h"


namespace mongo {

using boost::intrusive_ptr;

size_t writeCallback(char* buf, size_t size, size_t nmemb, void* up) {
    struct jsonBuffer* jsonBuf = (struct jsonBuffer*)up;
    size_t newBytes = size * nmemb;
    // This is n^2. Should use a vector instead of realloc
    jsonBuf->data = (char*)realloc(jsonBuf->data, jsonBuf->size + newBytes);
    memcpy(jsonBuf->data + jsonBuf->size, buf, newBytes);
    jsonBuf->size += newBytes;
    return newBytes;
}

// http://api.nytimes.com/svc/search/v2/articlesearch.json?q=obama&api-key=c2fede7bd9aea57c898f538e5ec0a1ee:6:68700045
DocumentSourceHttpGetJson::DocumentSourceHttpGetJson(
    const intrusive_ptr<ExpressionContext>& pExpCtx, BSONElement elem)
    : DocumentSource(pExpCtx), count(0) {
    // FIXME -  call curl_global_init first in a single-threaded init context.
    // FIXME - disable slowMS tracking
    curl = curl_easy_init();
    uassert(40058, "argument to $httpGET is not an object", elem.type() == Object);
    options = elem.Obj().getOwned();
}

DocumentSourceHttpGetJson::~DocumentSourceHttpGetJson() {
    curl_easy_cleanup(curl);
}

REGISTER_DOCUMENT_SOURCE(httpGET, DocumentSourceHttpGetJson::createFromBson);

boost::optional<Document> DocumentSourceHttpGetJson::getNext() {
    pExpCtx->checkForInterrupt();

    uri = options.getStringField("uri");
    std::string uriSeparator = "?";
    if (options.hasField("query")) {
        for (auto&& argument : options.getObjectField("query")) {
            std::string value = argument.String();
            std::string escaped = curl_easy_escape(curl, value.c_str(), value.length());
            uri += uriSeparator + argument.fieldName() + "=" + escaped;
            uriSeparator = "&";
        }
    }

    MutableDocument output;
    if (options.hasField("lookup")) {
        // OMG this is ugly. Nested layers of unreadable C++ crap.
        boost::optional<Document> input = pSource->getNext();
        if (!input) {
            log() << "no input from source";
            return boost::none;
        }
        log() << "Source: " << *input;
        output.reset(*input);
        // log() << "Output: " << output.freeze();
        auto lookupField = options.getStringField("lookup");
        Value queryValue = input->getNestedField(FieldPath(lookupField));
        log() << "lookup on Field: " << lookupField << " " << queryValue;
        uassert(40059, "$httpGET lookup is not an object", queryValue.getType() == Object);

        FieldIterator query(queryValue.getDocument());
        while (query.more()) {
            auto pair = query.next();
            std::string key = pair.first.toString();
            std::string value = pair.second.coerceToString();
            uri += uriSeparator + key + "=" + curl_easy_escape(curl, value.c_str(), value.length());
            uriSeparator = "&";
        }
    } else if (++count > 1)
        return boost::none;

    log() << "httpGET getNext " << uri;

    // Currently we do this all up front - for streaming many docs per doc of input,
    // would need to move to getNext
    // https://curl.haxx.se/libcurl/c/fopen.html
    jsonBuf.data = nullptr;
    jsonBuf.size = 0;
    curl_easy_setopt(curl, CURLOPT_URL, uri.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &jsonBuf);
    curl_easy_perform(curl);

    if (jsonBuf.size <= 0 || jsonBuf.data == nullptr)
        return boost::none;

    log() << "httpGET getNext curl returned " << jsonBuf.size << " bytes";

    BSONObj bson = fromjson(jsonBuf.data);
    if (options.hasField("subfield")) {
        BSONElement elem = bson.getFieldDotted(options.getStringField("subfield"));
        if (options.hasField("as"))
            output.setField(options.getStringField("as"), Value(elem));
        else
            output.reset(Document(elem.Obj()));
    } else {
        if (options.hasField("as")) {
            if (isArray(jsonBuf.data)) {
                std::vector<Value> values;
                for (auto&& elem : bson)
                    values.push_back(Value(elem));
                output.setField(options.getStringField("as"), Value(values));
            } else
                output.setField(options.getStringField("as"), Value(bson));
        } else
            output.reset(Document(bson));
    }

    // we need to add the stuff we already had here
    free(jsonBuf.data);
    return output.freeze();
}
}
