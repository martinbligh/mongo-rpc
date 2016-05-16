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

#include <netdb.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/util/log.h"

namespace mongo {

using boost::intrusive_ptr;

// This is a hack. Needs fixing for all the pointless IPv6 crappage.
int tcp_client(const char* hostname, uint16_t port) {
    struct hostent* host = gethostbyname(hostname);
    struct sockaddr_in addr;
    int sockfd = -1;

    uassert(40088, "$socketBSON bad hostname", host != NULL);
    uassert(40089, "$socketBSON not IPv4", (host->h_addrtype == AF_INET) && (host->h_length == 4));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    struct in_addr** addr_list = (struct in_addr**)host->h_addr_list;
    for (int i = 0; addr_list[i] != NULL; i++) {
        addr.sin_addr.s_addr = *((uint32_t*)addr_list[i]);
        if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
            continue;
        }
        if (connect(sockfd, (struct sockaddr*)&(addr), sizeof(addr)) == -1) {
            close(sockfd);
            sockfd = -1;
            continue;
        }
        break;
    }

    uassert(40090, "tcp connect failed in $socketBSON", sockfd >= 0);
    return sockfd;
}

DocumentSourceSocketBson::DocumentSourceSocketBson(const intrusive_ptr<ExpressionContext>& pExpCtx,
                                                   BSONElement elem)
    : DocumentSource(pExpCtx), streamDocsOut(false) {
    log() << "DocumentSourceSocketBson constructor";
    options = elem.Obj().getOwned();

    if (options.hasField("host"))
        host = options.getStringField("host");
    else
        host = "localhost";
    uint16_t port = options.getIntField("port");
    sockfd = tcp_client(host.c_str(), port);
    host += ":" + std::to_string(port);

    if (options.hasField("output"))
        streamDocsOut = options.getBoolField("output");

    if (options.hasField("initial"))
        writeToSocket(options.getObjectField("initial"));
    lookupInProgress = false;

    log() << "DocumentSourceSocketBson constructor done";
}

DocumentSourceSocketBson::~DocumentSourceSocketBson() {
    close(sockfd);
}

REGISTER_DOCUMENT_SOURCE(socketBSON, DocumentSourceSocketBson::createFromBson);

void DocumentSourceSocketBson::writeToSocket(BSONObj bson) {
    log() << "writeToSocket " << bson;
    // do we have to cope with partial writes on full buffer?
    ssize_t bytesWritten = write(sockfd, bson.objdata(), bson.objsize());
    uassert(40091, "$socketBSON bson write failed", bson.objsize() == bytesWritten);
    log() << "writeToSocket wrote " << bytesWritten << " vs " << bson.objsize();
}


boost::optional<BSONObj> DocumentSourceSocketBson::readFromSocket() {
    uint32_t bsonLength;
    ssize_t bytesRead;

    log() << "readFromSocket";
    bytesRead = recv(sockfd, &bsonLength, sizeof(bsonLength), MSG_PEEK);
    if (bytesRead != sizeof(bsonLength)) {
        log() << "readFromSocket bad peek: " << bytesRead;
        return boost::none;
    }
    log() << "readFromSocket reading bytes: " << bsonLength;
    char* bsonData = (char*)mongoMalloc(bsonLength);
    for (bytesRead = 0; bytesRead < bsonLength;) {
        ssize_t newBytesRead = read(sockfd, bsonData + bytesRead, bsonLength - bytesRead);
        if (newBytesRead <= 0) {
            log() << "readFromSocket bad read: " << bytesRead;
            free(bsonData);
            return boost::none;
        }
        bytesRead += newBytesRead;
    }

    BSONObj bson = BSONObj((char*)bsonData).getOwned();  // add error check
    free(bsonData);
    log() << "readFromSocket done " << bson;
    return bson;
}


boost::optional<Document> DocumentSourceSocketBson::getNext() {
    pExpCtx->checkForInterrupt();

    log() << "getNext";
    return options.hasField("localField") ? getNextLookup() : getNextNoLookup();
}


boost::optional<Document> DocumentSourceSocketBson::getNextLookup() {
    MutableDocument output;

    log() << "getNextLookup lookupInProgress: " << lookupInProgress;
    boost::optional<Document> input = pSource->getNext();
    if (!input)
        return boost::none;
    output.reset(*input);

    const char* lookupField = options.getStringField("localField");
    Document lookupDocument;
    if (lookupField[0] == '\0')  // blank => root of document
        lookupDocument = *input;
    else
        lookupDocument = input->getNestedField(FieldPath(lookupField)).getDocument();
    log() << "lookup on field: " << lookupField << " : " << lookupDocument;
    writeToSocket(lookupDocument.toBson());

    boost::optional<BSONObj> bson = readFromSocket();
    if (!bson)
        return boost::none;  // EOF

    output.setField(options.getStringField("as"), Value(*bson));
    return output.freeze();
}


boost::optional<Document> DocumentSourceSocketBson::getNextNoLookup() {
    log() << "getNextNoLookup";
    uint32_t bsonLength;
    if (recv(sockfd, &bsonLength, sizeof(bsonLength), MSG_PEEK) != sizeof(bsonLength))
        return boost::none;

    auto bson = readFromSocket();
    if (!bson)
        return boost::none;

    return Document(*bson);
}
}
