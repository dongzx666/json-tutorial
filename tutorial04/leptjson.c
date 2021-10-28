#ifdef _WINDOWS
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif
#include "leptjson.h"
#include <assert.h> /* assert() */
#include <errno.h>  /* errno, ERANGE */
#include <math.h>   /* HUGE_VAL */
#include <stdlib.h> /* NULL, malloc(), realloc(), free(), strtod() */
#include <string.h> /* memcpy() */

#ifndef LEPT_PARSE_STACK_INIT_SIZE
#define LEPT_PARSE_STACK_INIT_SIZE 256
#endif

#define EXPECT(c, ch)         \
  do {                        \
    assert(*c->json == (ch)); \
    c->json++;                \
  } while (0)
#define ISDIGIT(ch) ((ch) >= '0' && (ch) <= '9')
#define ISDIGIT1TO9(ch) ((ch) >= '1' && (ch) <= '9')
#define PUTC(c, ch)                                     \
  do {                                                  \
    *(char *)lept_context_push(c, sizeof(char)) = (ch); \
  } while (0)

typedef struct {
  const char *json;
  char *stack;
  size_t size, top;
} lept_context;

static void *lept_context_push(lept_context *c, size_t size) {
  void *ret;
  assert(size > 0);
  if (c->top + size >= c->size) {
    if (c->size == 0)
      c->size = LEPT_PARSE_STACK_INIT_SIZE;
    while (c->top + size >= c->size)
      c->size += c->size >> 1; /* c->size * 1.5 */
    c->stack = (char *)realloc(c->stack, c->size);
  }
  ret = c->stack + c->top;
  c->top += size;
  return ret;
}

static void *lept_context_pop(lept_context *c, size_t size) {
  assert(c->top >= size);
  return c->stack + (c->top -= size);
}

static void lept_parse_whitespace(lept_context *c) {
  const char *p = c->json;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    p++;
  c->json = p;
}

static int lept_parse_literal(lept_context *c, lept_value *v,
                              const char *literal, lept_type type) {
  size_t i;
  EXPECT(c, literal[0]);
  for (i = 0; literal[i + 1]; i++)
    if (c->json[i] != literal[i + 1])
      return LEPT_PARSE_INVALID_VALUE;
  c->json += i;
  v->type = type;
  return LEPT_PARSE_OK;
}

static int lept_parse_number(lept_context *c, lept_value *v) {
  const char *p = c->json;
  if (*p == '-')
    p++;
  if (*p == '0')
    p++;
  else {
    if (!ISDIGIT1TO9(*p))
      return LEPT_PARSE_INVALID_VALUE;
    for (p++; ISDIGIT(*p); p++)
      ;
  }
  if (*p == '.') {
    p++;
    if (!ISDIGIT(*p))
      return LEPT_PARSE_INVALID_VALUE;
    for (p++; ISDIGIT(*p); p++)
      ;
  }
  if (*p == 'e' || *p == 'E') {
    p++;
    if (*p == '+' || *p == '-')
      p++;
    if (!ISDIGIT(*p))
      return LEPT_PARSE_INVALID_VALUE;
    for (p++; ISDIGIT(*p); p++)
      ;
  }
  errno = 0;
  v->u.n = strtod(c->json, NULL);
  if (errno == ERANGE && (v->u.n == HUGE_VAL || v->u.n == -HUGE_VAL))
    return LEPT_PARSE_NUMBER_TOO_BIG;
  v->type = LEPT_NUMBER;
  c->json = p;
  return LEPT_PARSE_OK;
}

static const char *lept_parse_hex4(const char *p, unsigned *u) {
  char *end;
  *u = (unsigned)strtol(p, &end, 16);
  return end == p + 4 ? end : NULL;
}

static void lept_encode_utf8(lept_context *c, unsigned u) {
  /* TODO(fengyu): 直接低代理算不算错误？ [28-10-21] */
  if (u <= 0x7F)
    PUTC(c, u & 0xFF);
  else if (u <= 0x7FF) {
    /* 11000000(110xxxxx) -> 0xc0 */
    /* 10000000(10xxxxxx) -> 0x80 */
    /* 两个字节，字节2的位置是后六位，所以字节1的位置就是右移6位后的数 */
    /* x & 0xFF 清空高位，只留低8位 */
    /* x & 0x3F 清空高位，只留低6位 */
    PUTC(c, 0xC0 | ((u >> 6) & 0xFF));
    PUTC(c, 0x80 | (u & 0x3F));
  } else if (u <= 0xFFFF) {
    PUTC(c, 0xE0 | ((u >> 12) & 0xFF));
    PUTC(c, 0x80 | ((u >> 6) & 0x3F));
    PUTC(c, 0x80 | (u & 0x3F));
  } else if (u <= 0x10FFFF) {
    PUTC(c, 0xF0 | ((u >> 18) & 0xFF));
    PUTC(c, 0x80 | ((u >> 12) & 0x3F));
    PUTC(c, 0x80 | ((u >> 6) & 0x3F));
    PUTC(c, 0x80 | (u & 0x3F));
  } else {
    assert(0);
  }
}

#define STRING_ERROR(ret) \
  do {                    \
    c->top = head;        \
    return ret;           \
  } while (0)

static int lept_parse_string(lept_context *c, lept_value *v) {
  size_t head = c->top, len;
  unsigned u, u_low;
  const char *p;
  EXPECT(c, '\"');
  p = c->json;
  for (;;) {
    char ch = *p++;
    switch (ch) {
    case '\"':
      len = c->top - head;
      lept_set_string(v, (const char *)lept_context_pop(c, len), len);
      c->json = p;
      return LEPT_PARSE_OK;
    case '\\':
      switch (*p++) {
      case '\"':
        PUTC(c, '\"');
        break;
      case '\\':
        PUTC(c, '\\');
        break;
      case '/':
        PUTC(c, '/');
        break;
      case 'b':
        PUTC(c, '\b');
        break;
      case 'f':
        PUTC(c, '\f');
        break;
      case 'n':
        PUTC(c, '\n');
        break;
      case 'r':
        PUTC(c, '\r');
        break;
      case 't':
        PUTC(c, '\t');
        break;
      case 'u':
        if (!(p = lept_parse_hex4(p, &u))) {
          STRING_ERROR(LEPT_PARSE_INVALID_UNICODE_HEX);
        }
        /* \TODO surrogate handling */
        if (u >= 0xD800 && u <= 0xDBFF) {
          /*  高代理项是U+D800 至 U+DBFF 内的码点 */
          if (*p++ != '\\') {
            STRING_ERROR(LEPT_PARSE_INVALID_UNICODE_SURROGATE);
          }
          if (*p++ != 'u') {
            STRING_ERROR(LEPT_PARSE_INVALID_UNICODE_SURROGATE);
          }
          if (!(p = lept_parse_hex4(p, &u_low))) {
            STRING_ERROR(LEPT_PARSE_INVALID_UNICODE_HEX);
          }
          /* 低代理项是U+DC00 至 U+DFFF */
          if (u_low < 0xDC00 || u_low > 0xDFFF) {
            STRING_ERROR(LEPT_PARSE_INVALID_UNICODE_SURROGATE);
          }
          /* u = 0x10000 + (u - 0xD800) * 0x400 + (u_low - 0xDC00); */
          u = (((u - 0xD800) << 10) | (u_low - 0xDC00)) + 0x10000;
        }
        lept_encode_utf8(c, u);
        break;
      default:
        STRING_ERROR(LEPT_PARSE_INVALID_STRING_ESCAPE);
      }
      break;
    case '\0':
      STRING_ERROR(LEPT_PARSE_MISS_QUOTATION_MARK);
    default:
      if ((unsigned char)ch < 0x20)
        STRING_ERROR(LEPT_PARSE_INVALID_STRING_CHAR);
      PUTC(c, ch);
    }
  }
}

static int lept_parse_value(lept_context *c, lept_value *v) {
  switch (*c->json) {
  case 't':
    return lept_parse_literal(c, v, "true", LEPT_TRUE);
  case 'f':
    return lept_parse_literal(c, v, "false", LEPT_FALSE);
  case 'n':
    return lept_parse_literal(c, v, "null", LEPT_NULL);
  default:
    return lept_parse_number(c, v);
  case '"':
    return lept_parse_string(c, v);
  case '\0':
    return LEPT_PARSE_EXPECT_VALUE;
  }
}

int lept_parse(lept_value *v, const char *json) {
  lept_context c;
  int ret;
  assert(v != NULL);
  c.json = json;
  c.stack = NULL;
  c.size = c.top = 0;
  lept_init(v);
  lept_parse_whitespace(&c);
  if ((ret = lept_parse_value(&c, v)) == LEPT_PARSE_OK) {
    lept_parse_whitespace(&c);
    if (*c.json != '\0') {
      v->type = LEPT_NULL;
      ret = LEPT_PARSE_ROOT_NOT_SINGULAR;
    }
  }
  assert(c.top == 0);
  free(c.stack);
  return ret;
}

void lept_free(lept_value *v) {
  assert(v != NULL);
  if (v->type == LEPT_STRING)
    free(v->u.s.s);
  v->type = LEPT_NULL;
}

lept_type lept_get_type(const lept_value *v) {
  assert(v != NULL);
  return v->type;
}

int lept_get_boolean(const lept_value *v) {
  assert(v != NULL && (v->type == LEPT_TRUE || v->type == LEPT_FALSE));
  return v->type == LEPT_TRUE;
}

void lept_set_boolean(lept_value *v, int b) {
  lept_free(v);
  v->type = b ? LEPT_TRUE : LEPT_FALSE;
}

double lept_get_number(const lept_value *v) {
  assert(v != NULL && v->type == LEPT_NUMBER);
  return v->u.n;
}

void lept_set_number(lept_value *v, double n) {
  lept_free(v);
  v->u.n = n;
  v->type = LEPT_NUMBER;
}

const char *lept_get_string(const lept_value *v) {
  assert(v != NULL && v->type == LEPT_STRING);
  return v->u.s.s;
}

size_t lept_get_string_length(const lept_value *v) {
  assert(v != NULL && v->type == LEPT_STRING);
  return v->u.s.len;
}

void lept_set_string(lept_value *v, const char *s, size_t len) {
  assert(v != NULL && (s != NULL || len == 0));
  lept_free(v);
  v->u.s.s = (char *)malloc(len + 1);
  memcpy(v->u.s.s, s, len);
  v->u.s.s[len] = '\0';
  v->u.s.len = len;
  v->type = LEPT_STRING;
}
