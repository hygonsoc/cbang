/******************************************************************************\

          This file is part of the C! library.  A.K.A the cbang library.

                Copyright (c) 2003-2019, Cauldron Development LLC
                   Copyright (c) 2003-2017, Stanford University
                               All rights reserved.

         The C! library is free software: you can redistribute it and/or
        modify it under the terms of the GNU Lesser General Public License
       as published by the Free Software Foundation, either version 2.1 of
               the License, or (at your option) any later version.

        The C! library is distributed in the hope that it will be useful,
          but WITHOUT ANY WARRANTY; without even the implied warranty of
        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
                 Lesser General Public License for more details.

         You should have received a copy of the GNU Lesser General Public
                 License along with the C! library.  If not, see
                         <http://www.gnu.org/licenses/>.

        In addition, BSD licensing may be granted on a case by case basis
        by written permission from at least one of the copyright holders.
           You may request written permission by emailing the authors.

                  For information regarding this software email:
                                 Joseph Coffland
                          joseph@cauldrondevelopment.com

\******************************************************************************/

#include "Request.h"
#include "Buffer.h"
#include "BufferDevice.h"
#include "BufferEvent.h"
#include "Headers.h"
#include "Connection.h"

#include <cbang/Exception.h>
#include <cbang/Catch.h>
#include <cbang/openssl/SSL.h>
#include <cbang/log/Logger.h>
#include <cbang/http/Cookie.h>
#include <cbang/json/JSON.h>
#include <cbang/time/Time.h>

#include <event2/http.h>
#include <event2/http_struct.h>

#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/filter/bzip2.hpp>
namespace io = boost::iostreams;

using namespace cb::Event;
using namespace cb;
using namespace std;


namespace {
  void free_cb(struct evhttp_request *req, void *pr) {((Request *)pr)->freed();}


  struct FilteringOStreamWithRef : public io::filtering_ostream {
    SmartPointer<ostream> ref;
    virtual ~FilteringOStreamWithRef() {reset();}
  };


  SmartPointer<ostream> compressBufferStream
  (Event::Buffer buffer, Request::compression_t compression) {
    SmartPointer<ostream> target = new Event::BufferStream<>(buffer);
    SmartPointer<FilteringOStreamWithRef> out = new FilteringOStreamWithRef;

    switch (compression) {
    case Request::COMPRESS_ZLIB:  out->push(io::zlib_compressor()); break;
    case Request::COMPRESS_GZIP:  out->push(io::gzip_compressor()); break;
    case Request::COMPRESS_BZIP2: out->push(io::bzip2_compressor()); break;
    default: return target;
    }

    out->ref = target;
    out->push(*target);

    return out;
  }


  const char *getContentEncoding(Request::compression_t compression) {
    switch (compression) {
    case Request::COMPRESS_ZLIB:  return "zlib";
    case Request::COMPRESS_GZIP:  return "gzip";
    case Request::COMPRESS_BZIP2: return "bzip2";
    default: return 0;
    }
  }


  struct JSONWriter :
    Event::Buffer, SmartPointer<ostream>, public JSON::Writer {
    Request &req;

    JSONWriter(Request &req, unsigned indent, bool compact,
               Request::compression_t compression) :
      SmartPointer<ostream>(compressBufferStream(*this, compression)),
      JSON::Writer(*SmartPointer<ostream>::get(), indent, compact), req(req) {
      req.outSetContentEncoding(compression);
    }

    ~JSONWriter() {TRY_CATCH_ERROR(close(););}

    ostream &getStream() {return *SmartPointer<ostream>::get();}

    void close() {
      JSON::Writer::close();
      SmartPointer<ostream>::release();
      send(*this);
    }

    virtual void send(Event::Buffer &buffer) {req.send(buffer);}
  };
}


Request::Request(evhttp_request *req, bool deallocate) :
  req(req), deallocate(deallocate), id(0), user("anonymous"), incoming(false),
  finalized(false) {
  if (!req) THROW("Event request cannot be null");
  init();

  // Parse URI
  const char *uri = evhttp_request_get_uri(req);
  if (uri) this->uri = originalURI = uri;

  // Parse client IP
  evhttp_connection *_con = evhttp_request_get_connection(req);
  if (_con) {
    Connection con(_con, false);
    clientIP = con.getPeer();
  }

  // Log request
  LOG_INFO(1, "< " << getMethod() << " " << getURI());
  LOG_DEBUG(5, getInputHeaders() << '\n');
  LOG_DEBUG(6, getInputBuffer().hexdump() << '\n');
}


Request::Request(evhttp_request *req, const URI &uri, bool deallocate) :
  req(req), deallocate(deallocate), originalURI(uri), uri(uri),
  clientIP(uri.getHost(), uri.getPort()), incoming(false), finalized(false) {
  if (!req) THROW("Event request cannot be null");
  init();
}


Request::~Request() {
  if (req.isSet()) {
    evhttp_request_set_on_free_cb(req.access(), 0, 0);
    if (deallocate) evhttp_cancel_request(req.access()); // Also frees
  }
}


bool Request::hasConnection() const {
  return evhttp_request_get_connection(req.access());
}


Connection Request::getConnection() const {
  evhttp_connection *con = evhttp_request_get_connection(req.access());
  if (!con) THROW("Request does not have Connection");
  return Connection(con, false);
}


string Request::getLogPrefix() const {
  return String::printf("#%lld:", getID());
}


string Request::getSessionID(const string &cookie, const string &header) const {
  return inHas(header) ? inGet(header) : findCookie(cookie);
}


const string &Request::getUser() const {
  return (session.isNull() || !session->hasUser()) ? user : session->getUser();
}


void Request::setUser(const string &user) {
  this->user = user;
  if (!session.isNull()) session->setUser(user);
}


bool Request::isSecure() const {
  return hasConnection() && getConnection().getBufferEvent().hasSSL();
}


SSL Request::getSSL() const {return getConnection().getBufferEvent().getSSL();}


void Request::resetOutput() {
  if (finalized) THROW("Cannot reset output after Request has been finalized");
  getOutputBuffer().reset();
}


JSON::Dict &Request::parseJSONArgs() {
  Headers hdrs = getInputHeaders();

  if (hdrs.hasContentType() &&
      String::startsWith(hdrs.getContentType(), "application/json")) {

    Buffer buf = getInputBuffer();
    if (buf.getLength()) {
      Event::BufferStream<> stream(buf);
      JSON::Reader reader(stream);

      // Find start of dict & parse keys into request args
      if (reader.next() == '{') {
        JSON::ValuePtr argsPtr = JSON::ValuePtr::Phony(&args);
        JSON::Builder builder(argsPtr);
        reader.parseDict(builder);
      }
    }
  }

  return args;
}


JSON::Dict &Request::parseQueryArgs() {
  const URI &uri = getURI();
  for (URI::const_iterator it = uri.begin(); it != uri.end(); it++)
    insertArg(it->first, it->second);
  return args;
}


JSON::Dict &Request::parseArgs() {
  parseJSONArgs();
  parseQueryArgs();
  return args;
}


Version Request::getVersion() const {
  return Version(req->major, req->minor);
}


string Request::getHost() const {
  const char *host = evhttp_request_get_host(req.access());
  return host ? host : "";
}


URI Request::getURI(evhttp_request *req) {
  return evhttp_request_get_uri(req);
}


RequestMethod Request::getMethod() const {return getMethod(req.access());}


RequestMethod Request::getMethod(evhttp_request *req) {
  switch (evhttp_request_get_command(req)) {
  case EVHTTP_REQ_GET:     return HTTP_GET;
  case EVHTTP_REQ_POST:    return HTTP_POST;
  case EVHTTP_REQ_HEAD:    return HTTP_HEAD;
  case EVHTTP_REQ_PUT:     return HTTP_PUT;
  case EVHTTP_REQ_DELETE:  return HTTP_DELETE;
  case EVHTTP_REQ_OPTIONS: return HTTP_OPTIONS;
  case EVHTTP_REQ_TRACE:   return HTTP_TRACE;
  case EVHTTP_REQ_CONNECT: return HTTP_CONNECT;
  case EVHTTP_REQ_PATCH:   return HTTP_PATCH;
  default:                 return RequestMethod::HTTP_UNKNOWN;
  }
}


HTTPStatus Request::getResponseCode() const {
  return (HTTPStatus::enum_t)evhttp_request_get_response_code(req.access());
}


string Request::getResponseMessage() const {
  const char *s = evhttp_request_get_response_code_line(req.access());
  return s ? s : "";
}


string Request::getResponseLine() const {
  return SSTR("HTTP/" << (int)req->major << '.' << (int)req->minor << ' '
              << getResponseCode() << ' ' << getResponseMessage());
}


Headers Request::getInputHeaders() const {
  return evhttp_request_get_input_headers(req.access());
}


Headers Request::getOutputHeaders() const {
  return evhttp_request_get_output_headers(req.access());
}


bool Request::inHas(const string &name) const {
  return getInputHeaders().has(name);
}


string Request::inFind(const string &name) const {
  return getInputHeaders().find(name);
}


string Request::inGet(const string &name) const {
  return getInputHeaders().get(name);
}


void Request::inAdd(const string &name, const string &value) {
  getInputHeaders().add(name, value);
}


void Request::inSet(const string &name, const string &value) {
  getInputHeaders().set(name, value);
}


void Request::inRemove(const string &name) {getInputHeaders().remove(name);}


bool Request::outHas(const string &name) const {
  return getOutputHeaders().has(name);
}


string Request::outFind(const string &name) const {
  return getOutputHeaders().find(name);
}


string Request::outGet(const string &name) const {
  return getOutputHeaders().get(name);
}


void Request::outAdd(const string &name, const string &value) {
  getOutputHeaders().add(name, value);
}


void Request::outSet(const string &name, const string &value) {
  getOutputHeaders().set(name, value);
}


void Request::outRemove(const string &name) {
  getOutputHeaders().remove(name);
}


void Request::setPersistent(bool x) {
  if (getVersion() < Version(1, 1)) {
    if (x) outSet("Connection", "Keep-Alive");
    else outRemove("Connection");

  } else if (x) outRemove("Connection");
  else outSet("Connection", "close");
}


bool Request::hasContentType() const {
  return getOutputHeaders().hasContentType();
}


string Request::getContentType() const {
  return getOutputHeaders().getContentType();
}


void Request::setContentType(const string &contentType) {
  getOutputHeaders().setContentType(contentType);
}


void Request::guessContentType() {
  getOutputHeaders().guessContentType(uri.getExtension());
}


void Request::outSetContentEncoding(compression_t compression) {
  switch (compression) {
  case COMPRESS_ZLIB:
  case COMPRESS_GZIP:
  case COMPRESS_BZIP2:
    outSet("Content-Encoding", getContentEncoding(compression));
    break;
  default: break;
  }
}


Request::compression_t Request::getRequestedCompression() const {
  if (!inHas("Accept-Encoding")) return COMPRESS_NONE;

  vector<string> accept;
  String::tokenize(inGet("Accept-Encoding"), accept, ", \t");

  double maxQ = 0;
  double otherQ = 0;
  set<string> named;
  compression_t compression = COMPRESS_NONE;

  for (unsigned i = 0; i < accept.size(); i++) {
    double q = 1;
    string name = String::toLower(accept[i]);

    // Check for quality value
    size_t pos = name.find_first_of(';');
    if (pos != string::npos) {
      string arg = name.substr(pos + 1);
      name = name.substr(0, pos);

      if (2 < arg.length() && arg[0] == 'q' && arg[1] == '=') {
        q = String::parseDouble(arg.substr(2));
        if (name == "*") otherQ = q;
      }
    }

    named.insert(name);

    if (maxQ < q) {
      if (name == "identity")   compression = COMPRESS_NONE;
      else if (name == "gzip")  compression = COMPRESS_GZIP;
      else if (name == "zlib")  compression = COMPRESS_ZLIB;
      else if (name == "bzip2") compression = COMPRESS_BZIP2;
      else q = 0;
    }

    if (maxQ < q) maxQ = q;
  }

  // Currently, the only standard compression format we support is gzip, so
  // if the user specifies something like "*;q=1" and doesn't give gzip
  // an explicit quality value then we select gzip compression.
  if (maxQ < otherQ && named.find("gzip") == named.end())
    compression = COMPRESS_GZIP;

  return compression;
}


bool Request::hasCookie(const string &name) const {
  if (!inHas("Cookie")) return false;

  vector<string> cookies;
  String::tokenize(inGet("Cookie"), cookies, "; \t\n\r");

  for (unsigned i = 0; i < cookies.size(); i++)
    if (name == cookies[i].substr(0, cookies[i].find('='))) return true;

  return false;
}


string Request::findCookie(const string &name) const {
  if (inHas("Cookie")) {
    // Return only the first matching cookie
    vector<string> cookies;
    String::tokenize(inGet("Cookie"), cookies, "; \t\n\r");

    for (unsigned i = 0; i < cookies.size(); i++) {
      size_t pos = cookies[i].find('=');

      if (name == cookies[i].substr(0, pos))
        return pos == string::npos ? string() : cookies[i].substr(pos + 1);
    }
  }

  return "";
}


string Request::getCookie(const string &name) const {
  if (!hasCookie(name)) THROW("Cookie '" << name << "' not set");
  return findCookie(name);
}


void Request::setCookie(const string &name, const string &value,
                        const string &domain, const string &path,
                        uint64_t expires, uint64_t maxAge, bool httpOnly,
                        bool secure) {
  outAdd("Set-Cookie", HTTP::Cookie(name, value, domain, path, expires, maxAge,
                                    httpOnly, secure).toString());
}


void Request::setCache(uint32_t age) {
  const char *format = "%a, %d %b %Y %H:%M:%S GMT";
  string now = Time(format).toString();

  outSet("Date", now);

  if (age) {
    outSet("Cache-Control", "max-age=" + String(age));
    outSet("Expires", Time(Time::now() + age, format).toString());

  } else {
    outSet("Cache-Control", "max-age=0, no-cache, no-store");
    outSet("Expires", now);
  }
}


string Request::getInput() const {return getInputBuffer().toString();}
string Request::getOutput() const {return getOutputBuffer().toString();}


Event::Buffer Request::getInputBuffer() const {
  return Buffer(evhttp_request_get_input_buffer(req.access()), false);
}


Event::Buffer Request::getOutputBuffer() const {
  return Buffer(evhttp_request_get_output_buffer(req.access()), false);
}


SmartPointer<JSON::Value> Request::getInputJSON() const {
  Buffer buf = getInputBuffer();
  if (!buf.getLength()) return 0;
  Event::BufferStream<> stream(buf);
  return JSON::Reader(stream).parse();
}


SmartPointer<JSON::Value> Request::getJSONMessage() const {
  Headers hdrs = getInputHeaders();

  if (hdrs.hasContentType() &&
      String::startsWith(hdrs.getContentType(), "application/json"))
    return getInputJSON();

  SmartPointer<JSON::Value> msg;
  const URI &uri = getURI();

  if (!uri.empty()) {
    msg = new JSON::Dict;

    for (URI::const_iterator it = uri.begin(); it != uri.end(); it++)
      msg->insert(it->first, it->second);
  }

  return msg;
}


SmartPointer<JSON::Writer>
Request::getJSONWriter(unsigned indent, bool compact,
                       compression_t compression) {
  resetOutput();
  setContentType("application/json");

  return new JSONWriter(*this, indent, compact, compression);
}


SmartPointer<JSON::Writer> Request::getJSONWriter(compression_t compression) {
  return getJSONWriter(0, !getURI().has("pretty"), compression);
}


SmartPointer<JSON::Writer> Request::getJSONPWriter(const string &callback) {
  struct Writer : public JSONWriter {
    Writer(Request &req, const string &callback) :
      JSONWriter(req, 0, true, COMPRESS_NONE) {
      getStream() << callback << '(';
    }

    void close() {
      JSON::Writer::close();
      getStream() << ')';
      JSONWriter::close();
    }
  };

  resetOutput();
  setContentType("application/javascript");

  return new Writer(*this, callback);
}


SmartPointer<istream> Request::getInputStream() const {
  return new Event::BufferStream<>(getInputBuffer());
}


SmartPointer<ostream>
Request::getOutputStream(compression_t compression) {
  // Auto select compression type based on Accept-Encoding
  if (compression == COMPRESS_AUTO) compression = getRequestedCompression();
  outSetContentEncoding(compression);
  return compressBufferStream(getOutputBuffer(), compression);
}


void Request::sendError(int code) {
  finalize();
  evhttp_send_error(req.access(), code ? code : HTTP_INTERNAL_SERVER_ERROR, 0);
}


void Request::sendError(int code, const string &message) {
  resetOutput();
  setContentType("text/plain");
  send(message);
  sendError(code);
}


void Request::sendJSONError(int code, const string &message) {
  SmartPointer<JSON::Writer> writer = getJSONWriter();

  writer->beginList();
  writer->append("error");
  writer->append(message);
  writer->endList();
  writer->close();

  reply(code ? code : HTTP_INTERNAL_SERVER_ERROR);
}


void Request::send(const cb::Event::Buffer &buf) {getOutputBuffer().add(buf);}


void Request::send(const char *data, unsigned length) {
  getOutputBuffer().add(data, length);
}


void Request::send(const char *s) {getOutputBuffer().add(s);}
void Request::send(const std::string &s) {getOutputBuffer().add(s);}


void Request::sendFile(const std::string &path) {
  getOutputBuffer().addFile(path);
}


void Request::reply(int code) {
  finalize();
  evhttp_send_reply(req.access(), code,
                    HTTPStatus((HTTPStatus::enum_t)code).getDescription(), 0);
}


void Request::reply(const cb::Event::Buffer &buf) {reply(HTTP_OK, buf);}


void Request::reply(const char *data, unsigned length) {
  reply(HTTP_OK, data, length);
}


void Request::reply(int code, const string &s) {send(s); reply(code);}


void Request::reply(int code, const cb::Event::Buffer &buf) {
  finalize();
  evhttp_send_reply(req.access(), code,
                    HTTPStatus((HTTPStatus::enum_t)code).getDescription(),
                    buf.getBuffer());
}


void Request::reply(int code, const char *data, unsigned length) {
  reply(code, Buffer(data, length));
}


void Request::startChunked(int code) {
  evhttp_send_reply_start
    (req.access(), code, HTTPStatus((HTTPStatus::enum_t)code).getDescription());
}


void Request::sendChunk(const cb::Event::Buffer &buf) {
  evhttp_send_reply_chunk(req.access(), buf.getBuffer());
}


void Request::sendChunk(const char *data, unsigned length) {
  sendChunk(Buffer(data, length));
}


void Request::sendChunk(const string &s) {sendChunk(Buffer(s));}


SmartPointer<JSON::Writer> Request::getJSONChunkWriter() {
  struct Writer : public JSONWriter {
    Writer(Request &req) : JSONWriter(req, 0, true, COMPRESS_NONE) {}

    // From JSONWriter
    void send(Buffer &buffer) {req.sendChunk(buffer);}
  };

  return new Writer(*this);
}


void Request::endChunked() {
  finalize();
  evhttp_send_reply_end(req.access());
}


void Request::redirect(const URI &uri, int code) {
  outSet("Location", uri);
  outSet("Content-Length", "0");
  reply(code, "", 0);
}


void Request::cancel() {
  evhttp_cancel_request(req.access());
  finalized = true;
}


const char *Request::getErrorStr(int error) {
  switch (error) {
  case EVREQ_HTTP_TIMEOUT:        return "Timeout";
  case EVREQ_HTTP_EOF:            return "End of file";
  case EVREQ_HTTP_INVALID_HEADER: return "Invalid header";
  case EVREQ_HTTP_BUFFER_ERROR:   return "Buffer error";
  case EVREQ_HTTP_REQUEST_CANCEL: return "Request canceled";
  case EVREQ_HTTP_DATA_TOO_LONG:  return "Data too long";
  default:                        return "Unknown";
  }
}


void Request::freed() {
  req.release();
  SmartPointer<Request>::SelfRef::selfDeref();
}


void Request::init() {
  evhttp_request_set_on_free_cb(req.access(), free_cb, this);
  SmartPointer<Request>::SelfRef::selfRef();
}


void Request::finalize() {
  if (finalized) THROW("Request already finalized");
  finalized = true;

  if (!hasContentType()) guessContentType();

  // Log results
  LOG_DEBUG(5, getResponseLine() << '\n' << getOutputHeaders() << '\n');
  LOG_DEBUG(6, getOutputBuffer().hexdump() << '\n');
}
