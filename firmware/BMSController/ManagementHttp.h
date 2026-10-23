#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>

// One bounded request, parsed a few bytes at a time by the main loop.
struct ManagementHttp {
  static const size_t MAX_HEADER = 1024;
  static const size_t MAX_BODY = 1024;
  char buffer[MAX_HEADER + MAX_BODY + 1];
  char method[8];
  char path[32];
  size_t used = 0, bodyOffset = 0, contentLength = 0;
  bool complete = false;
  int error = 0;

  void reset() { used = bodyOffset = contentLength = 0; complete = false; error = 0; method[0] = path[0] = buffer[0] = 0; }
  const char* body() const { return buffer + bodyOffset; }
  static bool equal(const char* a, const char* b) {
    while (*a && *b) if (tolower(static_cast<unsigned char>(*a++)) != tolower(static_cast<unsigned char>(*b++))) return false;
    return *a == *b;
  }
  bool parseHeaders() {
    char* lineEnd = strstr(buffer, "\r\n");
    if (!lineEnd) return false;
    *lineEnd = 0;
    char* space = strchr(buffer, ' ');
    if (!space || size_t(space - buffer) >= sizeof(method)) return false;
    memcpy(method, buffer, space - buffer); method[space - buffer] = 0;
    char* target = space + 1;
    space = strchr(target, ' ');
    if (!space || size_t(space - target) >= sizeof(path) || *target != '/') return false;
    memcpy(path, target, space - target); path[space - target] = 0;
    if (strcmp(space + 1, "HTTP/1.1") && strcmp(space + 1, "HTTP/1.0")) return false;
    bool hasLength = false, json = false;
    for (char* line = lineEnd + 2; *line != '\r';) {
      lineEnd = strstr(line, "\r\n");
      if (!lineEnd) return false;
      *lineEnd = 0;
      char* colon = strchr(line, ':');
      if (!colon || colon == line) return false;
      *colon = 0;
      for (const char* byte = line; *byte; ++byte) {
        if (!isalnum(static_cast<unsigned char>(*byte)) && *byte != '-') return false;
      }
      char* value = colon + 1;
      while (*value == ' ' || *value == '\t') ++value;
      char* end = lineEnd;
      while (end > value && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
      if (equal(line, "Content-Length")) {
        if (hasLength || !*value) return false;
        hasLength = true;
        for (; *value; ++value) {
          if (*value < '0' || *value > '9') return false;
          contentLength = contentLength * 10 + (*value - '0');
          if (contentLength > MAX_BODY) return false;
        }
      } else if (equal(line, "Transfer-Encoding") || equal(line, "Expect")) return false;
      else if (equal(line, "Content-Type")) {
        char* separator = strchr(value, ';');
        if (separator) *separator = 0;
        if (json || !equal(value, "application/json")) return false;
        json = true;
      }
      line = lineEnd + 2;
    }
    if (!strcmp(method, "POST")) return hasLength && contentLength && json;
    return !contentLength;
  }
  void feed(char value) {
    if (complete || error) return;
    if (!value || used >= MAX_HEADER + MAX_BODY) { error = 400; return; }
    if (!bodyOffset && ((value == '\n' && (!used || buffer[used - 1] != '\r')) ||
        (used && buffer[used - 1] == '\r' && value != '\n') ||
        (static_cast<unsigned char>(value) < 32 && value != '\r' && value != '\n' && value != '\t'))) {
      error = 400; return;
    }
    buffer[used++] = value; buffer[used] = 0;
    if (!bodyOffset) {
      if (used >= 4 && !memcmp(buffer + used - 4, "\r\n\r\n", 4)) {
        bodyOffset = used;
        if (!parseHeaders()) { error = 400; return; }
        if (!contentLength) complete = true;
      } else if (used >= MAX_HEADER) error = 400;
    } else if (used == bodyOffset + contentLength) complete = true;
  }
};
