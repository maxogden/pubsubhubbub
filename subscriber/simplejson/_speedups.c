#include "Python.h"
#include "structmember.h"
#if PY_VERSION_HEX < 0x02060000 && !defined(Py_TYPE)
#define Py_TYPE(ob)     (((PyObject*)(ob))->ob_type)
#endif
#if PY_VERSION_HEX < 0x02050000 && !defined(PY_SSIZE_T_MIN)
typedef int Py_ssize_t;
#define PY_SSIZE_T_MAX INT_MAX
#define PY_SSIZE_T_MIN INT_MIN
#define PyInt_FromSsize_t PyInt_FromLong
#define PyInt_AsSsize_t PyInt_AsLong
#endif

#ifdef __GNUC__
#define UNUSED __attribute__((__unused__))
#else
#define UNUSED
#endif

#define DEFAULT_ENCODING "utf-8"

#define PyScanner_Check(op) PyObject_TypeCheck(op, &PyScannerType)
#define PyScanner_CheckExact(op) (Py_TYPE(op) == &PyScannerType)
#define PyEncoder_Check(op) PyObject_TypeCheck(op, &PyEncoderType)
#define PyEncoder_CheckExact(op) (Py_TYPE(op) == &PyEncoderType)

static PyTypeObject PyScannerType;
static PyTypeObject PyEncoderType;

typedef struct _PyScannerObject {
    PyObject_HEAD
    PyObject *encoding;
    PyObject *strict;
    PyObject *object_hook;
    PyObject *parse_float;
    PyObject *parse_int;
    PyObject *parse_constant;
} PyScannerObject;

static PyMemberDef scanner_members[] = {
    {"encoding", T_OBJECT, offsetof(PyScannerObject, encoding), READONLY, "encoding"},
    {"strict", T_OBJECT, offsetof(PyScannerObject, strict), READONLY, "strict"},
    {"object_hook", T_OBJECT, offsetof(PyScannerObject, object_hook), READONLY, "object_hook"},
    {"parse_float", T_OBJECT, offsetof(PyScannerObject, parse_float), READONLY, "parse_float"},
    {"parse_int", T_OBJECT, offsetof(PyScannerObject, parse_int), READONLY, "parse_int"},
    {"parse_constant", T_OBJECT, offsetof(PyScannerObject, parse_constant), READONLY, "parse_constant"},
    {NULL}
};

typedef struct _PyEncoderObject {
    PyObject_HEAD
    PyObject *markers;
    PyObject *defaultfn;
    PyObject *encoder;
    PyObject *indent;
    PyObject *key_separator;
    PyObject *item_separator;
    PyObject *sort_keys;
    PyObject *skipkeys;
    int fast_encode;
    int allow_nan;
} PyEncoderObject;

static PyMemberDef encoder_members[] = {
    {"markers", T_OBJECT, offsetof(PyEncoderObject, markers), READONLY, "markers"},
    {"default", T_OBJECT, offsetof(PyEncoderObject, defaultfn), READONLY, "default"},
    {"encoder", T_OBJECT, offsetof(PyEncoderObject, encoder), READONLY, "encoder"},
    {"indent", T_OBJECT, offsetof(PyEncoderObject, indent), READONLY, "indent"},
    {"key_separator", T_OBJECT, offsetof(PyEncoderObject, key_separator), READONLY, "key_separator"},
    {"item_separator", T_OBJECT, offsetof(PyEncoderObject, item_separator), READONLY, "item_separator"},
    {"sort_keys", T_OBJECT, offsetof(PyEncoderObject, sort_keys), READONLY, "sort_keys"},
    {"skipkeys", T_OBJECT, offsetof(PyEncoderObject, skipkeys), READONLY, "skipkeys"},
    {NULL}
};

static Py_ssize_t
ascii_escape_char(Py_UNICODE c, char *output, Py_ssize_t chars);
static PyObject *
ascii_escape_unicode(PyObject *pystr);
static PyObject *
ascii_escape_str(PyObject *pystr);
static PyObject *
py_encode_basestring_ascii(PyObject* self UNUSED, PyObject *pystr);
void init_speedups(void);
static PyObject *
scan_once_str(PyScannerObject *s, PyObject *pystr, Py_ssize_t idx);
static PyObject *
scan_once_unicode(PyScannerObject *s, PyObject *pystr, Py_ssize_t idx);
static PyObject *
_build_rval_index_tuple(PyObject *rval, Py_ssize_t idx);
static int
scanner_init(PyObject *self, PyObject *args, PyObject *kwds);
static void
scanner_dealloc(PyObject *self);
static int
encoder_init(PyObject *self, PyObject *args, PyObject *kwds);
static void
encoder_dealloc(PyObject *self);
static int
encoder_listencode_list(PyEncoderObject *s, PyObject *rval, PyObject *seq, Py_ssize_t indent_level);
static int
encoder_listencode_obj(PyEncoderObject *s, PyObject *rval, PyObject *obj, Py_ssize_t indent_level);
static int
encoder_listencode_dict(PyEncoderObject *s, PyObject *rval, PyObject *dct, Py_ssize_t indent_level);
static PyObject *
_encoded_const(PyObject *const);
static void
raise_errmsg(char *msg, PyObject *s, Py_ssize_t end);
static PyObject *
encoder_encode_string(PyEncoderObject *s, PyObject *obj);
static int
_convertPyInt_AsSsize_t(PyObject *o, Py_ssize_t *size_ptr);
static PyObject *
_convertPyInt_FromSsize_t(Py_ssize_t *size_ptr);
static PyObject *
encoder_encode_float(PyEncoderObject *s, PyObject *obj);

#define S_CHAR(c) (c >= ' ' && c <= '~' && c != '\\' && c != '"')
#define IS_WHITESPACE(c) (((c) == ' ') || ((c) == '\t') || ((c) == '\n') || ((c) == '\r'))

#define MIN_EXPANSION 6
#ifdef Py_UNICODE_WIDE
#define MAX_EXPANSION (2 * MIN_EXPANSION)
#else
#define MAX_EXPANSION MIN_EXPANSION
#endif

static int
_convertPyInt_AsSsize_t(PyObject *o, Py_ssize_t *size_ptr)
{
    *size_ptr = PyInt_AsSsize_t(o);
    if (*size_ptr == -1 && PyErr_Occurred());
        return 1;
    return 0;
}

static PyObject *
_convertPyInt_FromSsize_t(Py_ssize_t *size_ptr)
{
    return PyInt_FromSsize_t(*size_ptr);
}

static Py_ssize_t
ascii_escape_char(Py_UNICODE c, char *output, Py_ssize_t chars)
{
    output[chars++] = '\\';
    switch (c) {
        case '\\': output[chars++] = (char)c; break;
        case '"': output[chars++] = (char)c; break;
        case '\b': output[chars++] = 'b'; break;
        case '\f': output[chars++] = 'f'; break;
        case '\n': output[chars++] = 'n'; break;
        case '\r': output[chars++] = 'r'; break;
        case '\t': output[chars++] = 't'; break;
        default:
#ifdef Py_UNICODE_WIDE
            if (c >= 0x10000) {
                /* UTF-16 surrogate pair */
                Py_UNICODE v = c - 0x10000;
                c = 0xd800 | ((v >> 10) & 0x3ff);
                output[chars++] = 'u';
                output[chars++] = "0123456789abcdef"[(c >> 12) & 0xf];
                output[chars++] = "0123456789abcdef"[(c >>  8) & 0xf];
                output[chars++] = "0123456789abcdef"[(c >>  4) & 0xf];
                output[chars++] = "0123456789abcdef"[(c      ) & 0xf];
                c = 0xdc00 | (v & 0x3ff);
                output[chars++] = '\\';
            }
#endif
            output[chars++] = 'u';
            output[chars++] = "0123456789abcdef"[(c >> 12) & 0xf];
            output[chars++] = "0123456789abcdef"[(c >>  8) & 0xf];
            output[chars++] = "0123456789abcdef"[(c >>  4) & 0xf];
            output[chars++] = "0123456789abcdef"[(c      ) & 0xf];
    }
    return chars;
}

static PyObject *
ascii_escape_unicode(PyObject *pystr)
{
    Py_ssize_t i;
    Py_ssize_t input_chars;
    Py_ssize_t output_size;
    Py_ssize_t chars;
    PyObject *rval;
    char *output;
    Py_UNICODE *input_unicode;

    input_chars = PyUnicode_GET_SIZE(pystr);
    input_unicode = PyUnicode_AS_UNICODE(pystr);

    /* One char input can be up to 6 chars output, estimate 4 of these */
    output_size = 2 + (MIN_EXPANSION * 4) + input_chars;
    rval = PyString_FromStringAndSize(NULL, output_size);
    if (rval == NULL) {
        return NULL;
    }
    output = PyString_AS_STRING(rval);
    chars = 0;
    output[chars++] = '"';
    for (i = 0; i < input_chars; i++) {
        Py_UNICODE c = input_unicode[i];
        if (S_CHAR(c)) {
            output[chars++] = (char)c;
        }
        else {
            chars = ascii_escape_char(c, output, chars);
        }
        if (output_size - chars < (1 + MAX_EXPANSION)) {
            /* There's more than four, so let's resize by a lot */
            output_size *= 2;
            /* This is an upper bound */
            if (output_size > 2 + (input_chars * MAX_EXPANSION)) {
                output_size = 2 + (input_chars * MAX_EXPANSION);
            }
            if (_PyString_Resize(&rval, output_size) == -1) {
                return NULL;
            }
            output = PyString_AS_STRING(rval);
        }
    }
    output[chars++] = '"';
    if (_PyString_Resize(&rval, chars) == -1) {
        return NULL;
    }
    return rval;
}

static PyObject *
ascii_escape_str(PyObject *pystr)
{
    Py_ssize_t i;
    Py_ssize_t input_chars;
    Py_ssize_t output_size;
    Py_ssize_t chars;
    PyObject *rval;
    char *output;
    char *input_str;

    input_chars = PyString_GET_SIZE(pystr);
    input_str = PyString_AS_STRING(pystr);

    /* Fast path for a string that's already ASCII */
    for (i = 0; i < input_chars; i++) {
        Py_UNICODE c = (Py_UNICODE)(unsigned char)input_str[i];
        if (!S_CHAR(c)) {
            /* If we have to escape something, scan the string for unicode */
            Py_ssize_t j;
            for (j = i; j < input_chars; j++) {
                c = (Py_UNICODE)(unsigned char)input_str[j];
                if (c > 0x7f) {
                    /* We hit a non-ASCII character, bail to unicode mode */
                    PyObject *uni;
                    uni = PyUnicode_DecodeUTF8(input_str, input_chars, "strict");
                    if (uni == NULL) {
                        return NULL;
                    }
                    rval = ascii_escape_unicode(uni);
                    Py_DECREF(uni);
                    return rval;
                }
            }
            break;
        }
    }

    if (i == input_chars) {
        /* Input is already ASCII */
        output_size = 2 + input_chars;
    }
    else {
        /* One char input can be up to 6 chars output, estimate 4 of these */
        output_size = 2 + (MIN_EXPANSION * 4) + input_chars;
    }
    rval = PyString_FromStringAndSize(NULL, output_size);
    if (rval == NULL) {
        return NULL;
    }
    output = PyString_AS_STRING(rval);
    output[0] = '"';
    
    /* We know that everything up to i is ASCII already */
    chars = i + 1;
    memcpy(&output[1], input_str, i);

    for (; i < input_chars; i++) {
        Py_UNICODE c = (Py_UNICODE)(unsigned char)input_str[i];
        if (S_CHAR(c)) {
            output[chars++] = (char)c;
        }
        else {
            chars = ascii_escape_char(c, output, chars);
        }
        /* An ASCII char can't possibly expand to a surrogate! */
        if (output_size - chars < (1 + MIN_EXPANSION)) {
            /* There's more than four, so let's resize by a lot */
            output_size *= 2;
            if (output_size > 2 + (input_chars * MIN_EXPANSION)) {
                output_size = 2 + (input_chars * MIN_EXPANSION);
            }
            if (_PyString_Resize(&rval, output_size) == -1) {
                return NULL;
            }
            output = PyString_AS_STRING(rval);
        }
    }
    output[chars++] = '"';
    if (_PyString_Resize(&rval, chars) == -1) {
        return NULL;
    }
    return rval;
}

static void
raise_errmsg(char *msg, PyObject *s, Py_ssize_t end)
{
    static PyObject *errmsg_fn = NULL;
    PyObject *pymsg;
    if (errmsg_fn == NULL) {
        PyObject *decoder = PyImport_ImportModule("simplejson.decoder");
        if (decoder == NULL)
            return;
        errmsg_fn = PyObject_GetAttrString(decoder, "errmsg");
        Py_DECREF(decoder);
        if (errmsg_fn == NULL)
            return;
    }
    pymsg = PyObject_CallFunction(errmsg_fn, "(zOO&)", msg, s, _convertPyInt_FromSsize_t, &end);
    if (pymsg) {
        PyErr_SetObject(PyExc_ValueError, pymsg);
        Py_DECREF(pymsg);
    }
}

static PyObject *
join_list_string(PyObject *lst)
{
    static PyObject *joinfn = NULL;
    if (joinfn == NULL) {
        PyObject *ustr = PyString_FromStringAndSize(NULL, 0);
        if (ustr == NULL)
            return NULL;
        
        joinfn = PyObject_GetAttrString(ustr, "join");
        Py_DECREF(ustr);
        if (joinfn == NULL)
            return NULL;
    }
    return PyObject_CallFunctionObjArgs(joinfn, lst, NULL);
}

static PyObject *
_build_rval_index_tuple(PyObject *rval, Py_ssize_t idx) {
    /*
    steal a reference to rval, returns (rval, idx)
    */
    if (rval == NULL) {
        return NULL;
    }
    PyObject *tpl;
    PyObject *pyidx = PyInt_FromSsize_t(idx);
    if (pyidx == NULL) {
        Py_DECREF(rval);
        return NULL;
    }
    tpl = PyTuple_Pack(2, rval, pyidx);
    Py_DECREF(pyidx);
    Py_DECREF(rval);
    return tpl;
}

static PyObject *
scanstring_str(PyObject *pystr, Py_ssize_t end, char *encoding, int strict, Py_ssize_t *next_end_ptr)
{
    PyObject *rval;
    Py_ssize_t len = PyString_GET_SIZE(pystr);
    Py_ssize_t begin = end - 1;
    Py_ssize_t next = begin;
    int has_unicode = 0;
    char *buf = PyString_AS_STRING(pystr);
    PyObject *chunks = PyList_New(0);
    if (chunks == NULL) {
        goto bail;
    }
    if (end < 0 || len <= end) {
        PyErr_SetString(PyExc_ValueError, "end is out of bounds");
        goto bail;
    }
    while (1) {
        /* Find the end of the string or the next escape */
        Py_UNICODE c = 0;
        PyObject *chunk = NULL;
        for (next = end; next < len; next++) {
            c = (unsigned char)buf[next];
            if (c == '"' || c == '\\') {
                break;
            }
            else if (strict && c <= 0x1f) {
                raise_errmsg("Invalid control character at", pystr, next);
                goto bail;
            }
            else if (c > 0x7f) {
                has_unicode = 1;
            }
        }
        if (!(c == '"' || c == '\\')) {
            raise_errmsg("Unterminated string starting at", pystr, begin);
            goto bail;
        }
        /* Pick up this chunk if it's not zero length */
        if (next != end) {
            PyObject *strchunk = PyString_FromStringAndSize(&buf[end], next - end);
            if (strchunk == NULL) {
                goto bail;
            }
            if (has_unicode) {
                chunk = PyUnicode_FromEncodedObject(strchunk, encoding, NULL);
                Py_DECREF(strchunk);
                if (chunk == NULL) {
                    goto bail;
                }
            }
            else {
                chunk = strchunk;
            }
            if (PyList_Append(chunks, chunk)) {
                goto bail;
            }
            Py_DECREF(chunk);
        }
        next++;
        if (c == '"') {
            end = next;
            break;
        }
        if (next == len) {
            raise_errmsg("Unterminated string starting at", pystr, begin);
            goto bail;
        }
        c = buf[next];
        if (c != 'u') {
            /* Non-unicode backslash escapes */
            end = next + 1;
            switch (c) {
                case '"': break;
                case '\\': break;
                case '/': break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                default: c = 0;
            }
            if (c == 0) {
                raise_errmsg("Invalid \\escape", pystr, end - 2);
                goto bail;
            }
        }
        else {
            c = 0;
            next++;
            end = next + 4;
            if (end >= len) {
                raise_errmsg("Invalid \\uXXXX escape", pystr, next - 1);
                goto bail;
            }
            /* Decode 4 hex digits */
            for (; next < end; next++) {
                c <<= 4;
                Py_UNICODE digit = buf[next];
                switch (digit) {
                    case '0': case '1': case '2': case '3': case '4':
                    case '5': case '6': case '7': case '8': case '9':
                        c |= (digit - '0'); break;
                    case 'a': case 'b': case 'c': case 'd': case 'e':
                    case 'f':
                        c |= (digit - 'a' + 10); break;
                    case 'A': case 'B': case 'C': case 'D': case 'E':
                    case 'F':
                        c |= (digit - 'A' + 10); break;
                    default:
                        raise_errmsg("Invalid \\uXXXX escape", pystr, end - 5);
                        goto bail;
                }
            }
#ifdef Py_UNICODE_WIDE
            /* Surrogate pair */
            if ((c & 0xfc00) == 0xd800) {
                Py_UNICODE c2 = 0;
                if (end + 6 >= len) {
                    raise_errmsg("Unpaired high surrogate", pystr, end - 5);
                    goto bail;
                }
                if (buf[next++] != '\\' || buf[next++] != 'u') {
                    raise_errmsg("Unpaired high surrogate", pystr, end - 5);
                    goto bail;
                }
                end += 6;
                /* Decode 4 hex digits */
                for (; next < end; next++) {
                    c2 <<= 4;
                    Py_UNICODE digit = buf[next];
                    switch (digit) {
                        case '0': case '1': case '2': case '3': case '4':
                        case '5': case '6': case '7': case '8': case '9':
                            c2 |= (digit - '0'); break;
                        case 'a': case 'b': case 'c': case 'd': case 'e':
                        case 'f':
                            c2 |= (digit - 'a' + 10); break;
                        case 'A': case 'B': case 'C': case 'D': case 'E':
                        case 'F':
                            c2 |= (digit - 'A' + 10); break;
                        default:
                            raise_errmsg("Invalid \\uXXXX escape", pystr, end - 5);
                            goto bail;
                    }
                }
                if ((c2 & 0xfc00) != 0xdc00) {
                    raise_errmsg("Unpaired high surrogate", pystr, end - 5);
                    goto bail;
                }
                c = 0x10000 + (((c - 0xd800) << 10) | (c2 - 0xdc00));
            }
            else if ((c & 0xfc00) == 0xdc00) {
                raise_errmsg("Unpaired low surrogate", pystr, end - 5);
                goto bail;
            }
#endif
        }
        if (c > 0x7f) {
            has_unicode = 1;
        }
        if (has_unicode) {
            chunk = PyUnicode_FromUnicode(&c, 1);
            if (chunk == NULL) {
                goto bail;
            }
        }
        else {
            char c_char = Py_CHARMASK(c);
            chunk = PyString_FromStringAndSize(&c_char, 1);
            if (chunk == NULL) {
                goto bail;
            }
        }
        if (PyList_Append(chunks, chunk)) {
            goto bail;
        }
        Py_DECREF(chunk);
    }

    rval = join_list_string(chunks);
    if (rval == NULL) {
        goto bail;
    }
    Py_DECREF(chunks);
    chunks = NULL;
    *next_end_ptr = end;
    return rval;
bail:
    *next_end_ptr = -1;
    Py_XDECREF(chunks);
    return NULL;
}


static PyObject *
scanstring_unicode(PyObject *pystr, Py_ssize_t end, int strict, Py_ssize_t *next_end_ptr)
{
    PyObject *rval;
    Py_ssize_t len = PyUnicode_GET_SIZE(pystr);
    Py_ssize_t begin = end - 1;
    Py_ssize_t next = begin;
    const Py_UNICODE *buf = PyUnicode_AS_UNICODE(pystr);
    PyObject *chunks = PyList_New(0);
    if (chunks == NULL) {
        goto bail;
    }
    if (end < 0 || len <= end) {
        PyErr_SetString(PyExc_ValueError, "end is out of bounds");
        goto bail;
    }
    while (1) {
        /* Find the end of the string or the next escape */
        Py_UNICODE c = 0;
        PyObject *chunk = NULL;
        for (next = end; next < len; next++) {
            c = buf[next];
            if (c == '"' || c == '\\') {
                break;
            }
            else if (strict && c <= 0x1f) {
                raise_errmsg("Invalid control character at", pystr, next);
                goto bail;
            }
        }
        if (!(c == '"' || c == '\\')) {
            raise_errmsg("Unterminated string starting at", pystr, begin);
            goto bail;
        }
        /* Pick up this chunk if it's not zero length */
        if (next != end) {
            chunk = PyUnicode_FromUnicode(&buf[end], next - end);
            if (chunk == NULL) {
                goto bail;
            }
            if (PyList_Append(chunks, chunk)) {
                goto bail;
            }
            Py_DECREF(chunk);
        }
        next++;
        if (c == '"') {
            end = next;
            break;
        }
        if (next == len) {
            raise_errmsg("Unterminated string starting at", pystr, begin);
            goto bail;
        }
        c = buf[next];
        if (c != 'u') {
            /* Non-unicode backslash escapes */
            end = next + 1;
            switch (c) {
                case '"': break;
                case '\\': break;
                case '/': break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                default: c = 0;
            }
            if (c == 0) {
                raise_errmsg("Invalid \\escape", pystr, end - 2);
                goto bail;
            }
        }
        else {
            c = 0;
            next++;
            end = next + 4;
            if (end >= len) {
                raise_errmsg("Invalid \\uXXXX escape", pystr, next - 1);
                goto bail;
            }
            /* Decode 4 hex digits */
            for (; next < end; next++) {
                c <<= 4;
                Py_UNICODE digit = buf[next];
                switch (digit) {
                    case '0': case '1': case '2': case '3': case '4':
                    case '5': case '6': case '7': case '8': case '9':
                        c |= (digit - '0'); break;
                    case 'a': case 'b': case 'c': case 'd': case 'e':
                    case 'f':
                        c |= (digit - 'a' + 10); break;
                    case 'A': case 'B': case 'C': case 'D': case 'E':
                    case 'F':
                        c |= (digit - 'A' + 10); break;
                    default:
                        raise_errmsg("Invalid \\uXXXX escape", pystr, end - 5);
                        goto bail;
                }
            }
#ifdef Py_UNICODE_WIDE
            /* Surrogate pair */
            if ((c & 0xfc00) == 0xd800) {
                Py_UNICODE c2 = 0;
                if (end + 6 >= len) {
                    raise_errmsg("Unpaired high surrogate", pystr, end - 5);
                    goto bail;
                }
                if (buf[next++] != '\\' || buf[next++] != 'u') {
                    raise_errmsg("Unpaired high surrogate", pystr, end - 5);
                    goto bail;
                }
                end += 6;
                /* Decode 4 hex digits */
                for (; next < end; next++) {
                    c2 <<= 4;
                    Py_UNICODE digit = buf[next];
                    switch (digit) {
                        case '0': case '1': case '2': case '3': case '4':
                        case '5': case '6': case '7': case '8': case '9':
                            c2 |= (digit - '0'); break;
                        case 'a': case 'b': case 'c': case 'd': case 'e':
                        case 'f':
                            c2 |= (digit - 'a' + 10); break;
                        case 'A': case 'B': case 'C': case 'D': case 'E':
                        case 'F':
                            c2 |= (digit - 'A' + 10); break;
                        default:
                            raise_errmsg("Invalid \\uXXXX escape", pystr, end - 5);
                            goto bail;
                    }
                }
                if ((c2 & 0xfc00) != 0xdc00) {
                    raise_errmsg("Unpaired high surrogate", pystr, end - 5);
                    goto bail;
                }
                c = 0x10000 + (((c - 0xd800) << 10) | (c2 - 0xdc00));
            }
            else if ((c & 0xfc00) == 0xdc00) {
                raise_errmsg("Unpaired low surrogate", pystr, end - 5);
                goto bail;
            }
#endif
        }
        chunk = PyUnicode_FromUnicode(&c, 1);
        if (chunk == NULL) {
            goto bail;
        }
        if (PyList_Append(chunks, chunk)) {
            goto bail;
        }
        Py_DECREF(chunk);
    }

    rval = join_list_string(chunks);
    if (rval == NULL) {
        goto bail;
    }
    Py_DECREF(chunks);
    *next_end_ptr = end;
    return rval;
bail:
    *next_end_ptr = -1;
    Py_XDECREF(chunks);
    return NULL;
}

PyDoc_STRVAR(pydoc_scanstring,
    "scanstring(basestring, end, encoding) -> (str, end)\n"
    "\n"
    "..."
);

static PyObject *
py_scanstring(PyObject* self UNUSED, PyObject *args)
{
    PyObject *pystr;
    PyObject *rval;
    Py_ssize_t end;
    Py_ssize_t next_end = -1;
    char *encoding = NULL;
    int strict = 0;
    if (!PyArg_ParseTuple(args, "OO&|zi:scanstring", &pystr, _convertPyInt_AsSsize_t, &end, &encoding, &strict)) {
        return NULL;
    }
    if (encoding == NULL) {
        encoding = DEFAULT_ENCODING;
    }
    if (PyString_Check(pystr)) {
        rval = scanstring_str(pystr, end, encoding, strict, &next_end);
    }
    else if (PyUnicode_Check(pystr)) {
        rval = scanstring_unicode(pystr, end, strict, &next_end);
    }
    else {
        PyErr_Format(PyExc_TypeError,
                     "first argument must be a string, not %.80s",
                     Py_TYPE(pystr)->tp_name);
        return NULL;
    }
    return _build_rval_index_tuple(rval, next_end);
}

PyDoc_STRVAR(pydoc_encode_basestring_ascii,
    "encode_basestring_ascii(basestring) -> str\n"
    "\n"
    "..."
);

static PyObject *
py_encode_basestring_ascii(PyObject* self UNUSED, PyObject *pystr)
{
    /* METH_O */
    if (PyString_Check(pystr)) {
        return ascii_escape_str(pystr);
    }
    else if (PyUnicode_Check(pystr)) {
        return ascii_escape_unicode(pystr);
    }
    else {
        PyErr_Format(PyExc_TypeError,
                     "first argument must be a string, not %.80s",
                     Py_TYPE(pystr)->tp_name);
        return NULL;
    }
}

static void
scanner_dealloc(PyObject *self)
{
    assert(PyScanner_Check(self));
    PyScannerObject *s = (PyScannerObject *)self;
    Py_XDECREF(s->encoding);
    Py_XDECREF(s->strict);
    Py_XDECREF(s->object_hook);
    Py_XDECREF(s->parse_float);
    Py_XDECREF(s->parse_int);
    Py_XDECREF(s->parse_constant);
    s->encoding = NULL;
    s->strict = NULL;
    s->object_hook = NULL;
    s->parse_float = NULL;
    s->parse_int = NULL;
    s->parse_constant = NULL;
    self->ob_type->tp_free(self);
}

static PyObject *
_parse_object_str(PyScannerObject *s, PyObject *pystr, Py_ssize_t idx) {
    char *str = PyString_AS_STRING(pystr);
    Py_ssize_t end_idx = PyString_GET_SIZE(pystr) - 1;
    PyObject *tpl = NULL;
    PyObject *rval = PyDict_New();
    PyObject *key = NULL;
    char *encoding = PyString_AS_STRING(s->encoding);
    int strict = PyObject_IsTrue(s->strict);
    Py_ssize_t next_idx;
    if (rval == NULL)
        return NULL;

    /* skip whitespace after { */
    while (idx <= end_idx && IS_WHITESPACE(str[idx])) idx++;

    /* only loop if the object is non-empty */
    if (idx <= end_idx && str[idx] != '}') {
        while (idx <= end_idx) {
            /* read key */
            if (str[idx] != '"') {
                raise_errmsg("Expecting property name", pystr, idx);
                goto bail;
            }
            key = scanstring_str(pystr, idx + 1, encoding, strict, &next_idx);
            if (key == NULL)
                goto bail;
            Py_INCREF(key);
            idx = next_idx;
            
            /* skip whitespace between key and : delimiter, read :, skip whitespace */
            while (idx <= end_idx && IS_WHITESPACE(str[idx])) idx++;
            if (idx > end_idx || str[idx] != ':') {
                raise_errmsg("Expecting : delimiter", pystr, idx);
                goto bail;
            }
            idx++;
            while (idx <= end_idx && IS_WHITESPACE(str[idx])) idx++;
            
            /* read any JSON data type and de-tuplefy the (rval, idx) */
            tpl = scan_once_str(s, pystr, idx);
            if (tpl == NULL)
                goto bail;
            next_idx = PyInt_AsSsize_t(PyTuple_GET_ITEM(tpl, 1));
            if (next_idx == -1 && PyErr_Occurred())
                goto bail;
            if (PyDict_SetItem(rval, key, PyTuple_GET_ITEM(tpl, 0)) == -1)
                goto bail;
            Py_DECREF(tpl);
            idx = next_idx;
            tpl = NULL;
            
            /* skip whitespace before } or , */
            while (idx <= end_idx && IS_WHITESPACE(str[idx])) idx++;

            /* bail if the object is closed or we didn't get the , delimiter */
            if (idx > end_idx) break;
            if (str[idx] == '}') {
                break;
            }
            else if (str[idx] != ',') {
                raise_errmsg("Expecting , delimiter", pystr, idx);
                goto bail;
            }
            idx++;

            /* skip whitespace after , delimiter */
            while (idx <= end_idx && IS_WHITESPACE(str[idx])) idx++;
        }
    }
    /* verify that idx < end_idx, str[idx] should be '}' */
    if (idx > end_idx || str[idx] != '}') {
        raise_errmsg("Expecting object", pystr, end_idx);
        goto bail;
    }
    /* if object_hook is not None: rval = object_hook(rval) */
    if (s->object_hook != Py_None) {
        tpl = PyObject_CallFunctionObjArgs(s->object_hook, rval, NULL);
        if (tpl == NULL)
            goto bail;
        Py_DECREF(rval);
        rval = tpl;
        tpl = NULL;
    }
    return _build_rval_index_tuple(rval, idx + 1);
bail:
    Py_XDECREF(key);
    Py_XDECREF(tpl);
    Py_DECREF(rval);
    return NULL;    
}

static PyObject *
_parse_object_unicode(PyScannerObject *s, PyObject *pystr, Py_ssize_t idx) {
    Py_UNICODE *str = PyUnicode_AS_UNICODE(pystr);
    Py_ssize_t end_idx = PyUnicode_GET_SIZE(pystr) - 1;
    PyObject *tpl = NULL;
    PyObject *rval = PyDict_New();
    PyObject *key = NULL;
    int strict = PyObject_IsTrue(s->strict);
    Py_ssize_t next_idx;
    if (rval == NULL)
        return NULL;
    
    /* skip whitespace after { */
    while (idx <= end_idx && IS_WHITESPACE(str[idx])) idx++;

    /* only loop if the object is non-empty */
    if (idx <= end_idx && str[idx] != '}') {
        while (idx <= end_idx) {
            /* read key */
            if (str[idx] != '"') {
                raise_errmsg("Expecting property name", pystr, idx);
                goto bail;
            }
            key = scanstring_unicode(pystr, idx + 1, strict, &next_idx);
            if (key == NULL)
                goto bail;
            idx = next_idx;

            /* skip whitespace between key and : delimiter, read :, skip whitespace */
            while (idx <= end_idx && IS_WHITESPACE(str[idx])) idx++;
            if (idx > end_idx || str[idx] != ':') {
                raise_errmsg("Expecting : delimiter", pystr, idx);
                goto bail;
            }
            idx++;
            while (idx <= end_idx && IS_WHITESPACE(str[idx])) idx++;
            
            /* read any JSON term and de-tuplefy the (rval, idx) */
            tpl = scan_once_unicode(s, pystr, idx);
            if (tpl == NULL)
                goto bail;
            next_idx = PyInt_AsSsize_t(PyTuple_GET_ITEM(tpl, 1));
            if (next_idx == -1 && PyErr_Occurred())
                goto bail;
            if (PyDict_SetItem(rval, key, PyTuple_GET_ITEM(tpl, 0)) == -1)
                goto bail;
            Py_DECREF(tpl);
            idx = next_idx;
            tpl = NULL;

            /* skip whitespace before } or , */
            while (idx <= end_idx && IS_WHITESPACE(str[idx])) idx++;

            /* bail if the object is closed or we didn't get the , delimiter */
            if (idx > end_idx) break;
            if (str[idx] == '}') {
                break;
            }
            else if (str[idx] != ',') {
                raise_errmsg("Expecting , delimiter", pystr, idx);
                goto bail;
            }
            idx++;

            /* skip whitespace after , delimiter */
            while (idx <= end_idx && IS_WHITESPACE(str[idx])) idx++;
        }
    }

    /* verify that idx < end_idx, str[idx] should be '}' */
    if (idx > end_idx || str[idx] != '}') {
        raise_errmsg("Expecting object", pystr, end_idx);
        goto bail;
    }

    /* if object_hook is not None: rval = object_hook(rval) */
    if (s->object_hook != Py_None) {
        tpl = PyObject_CallFunctionObjArgs(s->object_hook, rval, NULL);
        if (tpl == NULL)
            goto bail;
        Py_DECREF(rval);
        rval = tpl;
        tpl = NULL;
    }
    return _build_rval_index_tuple(rval, idx + 1);
bail:
    Py_XDECREF(key);
    Py_XDECREF(tpl);
    Py_DECREF(rval);
    return NULL;
}

static PyObject *
_parse_array_str(PyScannerObject *s, PyObject *pystr, Py_ssize_t idx) {
    char *str = PyString_AS_STRING(pystr);
    Py_ssize_t end_idx = PyString_GET_SIZE(pystr) - 1;
    PyObject *tpl = NULL;
    PyObject *rval = PyList_New(0);
    Py_ssize_t next_idx;
    if (rval == NULL)
        return NULL;

    /* skip whitespace after [ */
    while (idx <= end_idx && IS_WHITESPACE(str[idx])) idx++;

    /* only loop if the array is non-empty */
    if (idx <= end_idx && str[idx] != ']') {
        while (idx <= end_idx) {

            /* read any JSON term and de-tuplefy the (rval, idx) */
            tpl = scan_once_str(s, pystr, idx);
            if (tpl == NULL)
                goto bail;
            next_idx = PyInt_AsSsize_t(PyTuple_GET_ITEM(tpl, 1));
            if (next_idx == -1 && PyErr_Occurred())
                goto bail;
            if (PyList_Append(rval, PyTuple_GET_ITEM(tpl, 0)) == -1)
                goto bail;
            Py_DECREF(tpl);
            idx = next_idx;
            tpl = NULL;
            
            /* skip whitespace between term and , */
            while (idx <= end_idx && IS_WHITESPACE(str[idx])) idx++;

            /* bail if the array is closed or we didn't get the , delimiter */
            if (idx > end_idx) break;
            if (str[idx] == ']') {
                break;
            }
            else if (str[idx] != ',') {
                raise_errmsg("Expecting , delimiter", pystr, idx);
                goto bail;
            }
            idx++;
            
            /* skip whitespace after , */
            while (idx <= end_idx && IS_WHITESPACE(str[idx])) idx++;
        }
    }

    /* verify that idx < end_idx, str[idx] should be ']' */
    if (idx > end_idx || str[idx] != ']') {
        raise_errmsg("Expecting object", pystr, end_idx);
        goto bail;
    }
    return _build_rval_index_tuple(rval, idx + 1);
bail:
    Py_XDECREF(tpl);
    Py_DECREF(rval);
    return NULL;
}

static PyObject *
_parse_array_unicode(PyScannerObject *s, PyObject *pystr, Py_ssize_t idx) {
    Py_UNICODE *str = PyUnicode_AS_UNICODE(pystr);
    Py_ssize_t end_idx = PyUnicode_GET_SIZE(pystr) - 1;
    PyObject *tpl = NULL;
    PyObject *rval = PyList_New(0);
    Py_ssize_t next_idx;
    if (rval == NULL)
        return NULL;

    /* skip whitespace after [ */
    while (idx <= end_idx && IS_WHITESPACE(str[idx])) idx++;

    /* only loop if the array is non-empty */
    if (idx <= end_idx && str[idx] != ']') {
        while (idx <= end_idx) {

            /* read any JSON term and de-tuplefy the (rval, idx) */
            tpl = scan_once_unicode(s, pystr, idx);
            if (tpl == NULL)
                goto bail;
            next_idx = PyInt_AsSsize_t(PyTuple_GET_ITEM(tpl, 1));
            if (next_idx == -1 && PyErr_Occurred())
                goto bail;
            if (PyList_Append(rval, PyTuple_GET_ITEM(tpl, 0)) == -1)
                goto bail;
            Py_DECREF(tpl);
            idx = next_idx;
            tpl = NULL;

            /* skip whitespace between term and , */
            while (idx <= end_idx && IS_WHITESPACE(str[idx])) idx++;

            /* bail if the array is closed or we didn't get the , delimiter */
            if (idx > end_idx) break;
            if (str[idx] == ']') {
                break;
            }
            else if (str[idx] != ',') {
                raise_errmsg("Expecting , delimiter", pystr, idx);
                goto bail;
            }
            idx++;

            /* skip whitespace after , */
            while (idx <= end_idx && IS_WHITESPACE(str[idx])) idx++;
        }
    }

    /* verify that idx < end_idx, str[idx] should be ']' */
    if (idx > end_idx || str[idx] != ']') {
        raise_errmsg("Expecting object", pystr, end_idx);
        goto bail;
    }
    return _build_rval_index_tuple(rval, idx + 1);
bail:
    Py_XDECREF(tpl);
    Py_DECREF(rval);
    return NULL;
}

static PyObject *
_parse_constant(PyScannerObject *s, char *constant, Py_ssize_t idx) {
    PyObject *cstr;
    PyObject *rval;
    /* constant is "NaN", "Infinity", or "-Infinity" */
    cstr = PyString_InternFromString(constant);
    if (cstr == NULL)
        return NULL;

    /* rval = parse_constant(constant) */
    rval = PyObject_CallFunctionObjArgs(s->parse_constant, cstr, NULL);
    idx += PyString_GET_SIZE(cstr);
    Py_DECREF(cstr);
    return _build_rval_index_tuple(rval, idx);
}

static PyObject *
_match_number_str(PyScannerObject *s, PyObject *pystr, Py_ssize_t start) {
    char *str = PyString_AS_STRING(pystr);
    Py_ssize_t end_idx = PyString_GET_SIZE(pystr) - 1;
    Py_ssize_t idx = start;
    int is_float = 0;
    PyObject *rval;
    PyObject *numstr;
    
    /* read a sign if it's there, make sure it's not the end of the string */
    if (str[idx] == '-') {
        idx++;
        if (idx > end_idx) {
            PyErr_SetNone(PyExc_StopIteration);
            return NULL;
        }
    }

    /* read as many integer digits as we find as long as it doesn't start with 0 */
    if (str[idx] >= '1' && str[idx] <= '9') {
        idx++;
        while (idx <= end_idx && str[idx] >= '0' && str[idx] <= '9') idx++;
    }
    /* if it starts with 0 we only expect one integer digit */
    else if (str[idx] == '0') {
        idx++;
    }
    /* no integer digits, error */
    else {
        PyErr_SetNone(PyExc_StopIteration);
        return NULL;
    }
    
    /* if the next char is '.' followed by a digit then read all float digits */
    if (idx < end_idx && str[idx] == '.' && str[idx + 1] >= '0' && str[idx + 1] <= '9') {
        is_float = 1;
        idx += 2;
        while (idx < end_idx && str[idx] >= '0' && str[idx] <= '9') idx++;
    }

    /* if the next char is 'e' or 'E' then maybe read the exponent (or backtrack) */
    if (idx < end_idx && (str[idx] == 'e' || str[idx] == 'E')) {

        /* save the index of the 'e' or 'E' just in case we need to backtrack */
        Py_ssize_t e_start = idx;
        idx++;

        /* read an exponent sign if present */
        if (idx < end_idx && (str[idx] == '-' || str[idx] == '+')) idx++;

        /* read all digits */
        while (idx <= end_idx && str[idx] >= '0' && str[idx] <= '9') idx++;

        /* if we got a digit, then parse as float. if not, backtrack */
        if (str[idx - 1] >= '0' && str[idx - 1] <= '9') {
            is_float = 1;
        }
        else {
            idx = e_start;
        }
    }
    
    /* copy the section we determined to be a number */
    numstr = PyString_FromStringAndSize(&str[start], idx - start);
    if (numstr == NULL)
        return NULL;
    if (is_float) {
        /* parse as a float using a fast path if available, otherwise call user defined method */
        if (s->parse_float != (PyObject *)&PyFloat_Type) {
            rval = PyObject_CallFunctionObjArgs(s->parse_float, numstr, NULL);
        }
        else {
            rval = PyFloat_FromDouble(PyOS_ascii_atof(PyString_AS_STRING(numstr)));
        }
    }
    else {
        /* parse as an int using a fast path if available, otherwise call user defined method */
        if (s->parse_int != (PyObject *)&PyInt_Type) {
            rval = PyObject_CallFunctionObjArgs(s->parse_int, numstr, NULL);
        }
        else {
            rval = PyInt_FromString(PyString_AS_STRING(numstr), NULL, 10);
        }
    }
    Py_DECREF(numstr);
    return _build_rval_index_tuple(rval, idx);
}

static PyObject *
_match_number_unicode(PyScannerObject *s, PyObject *pystr, Py_ssize_t start) {
    Py_UNICODE *str = PyUnicode_AS_UNICODE(pystr);
    Py_ssize_t end_idx = PyUnicode_GET_SIZE(pystr) - 1;
    Py_ssize_t idx = start;
    int is_float = 0;
    PyObject *rval;
    PyObject *numstr;

    /* read a sign if it's there, make sure it's not the end of the string */
    if (str[idx] == '-') {
        idx++;
        if (idx > end_idx) {
            PyErr_SetNone(PyExc_StopIteration);
            return NULL;
        }
    }

    /* read as many integer digits as we find as long as it doesn't start with 0 */
    if (str[idx] >= '1' && str[idx] <= '9') {
        idx++;
        while (idx <= end_idx && str[idx] >= '0' && str[idx] <= '9') idx++;
    }
    /* if it starts with 0 we only expect one integer digit */
    else if (str[idx] == '0') {
        idx++;
    }
    /* no integer digits, error */
    else {
        PyErr_SetNone(PyExc_StopIteration);
        return NULL;
    }

    /* if the next char is '.' followed by a digit then read all float digits */
    if (idx < end_idx && str[idx] == '.' && str[idx + 1] >= '0' && str[idx + 1] <= '9') {
        is_float = 1;
        idx += 2;
        while (idx < end_idx && str[idx] >= '0' && str[idx] <= '9') idx++;
    }

    /* if the next char is 'e' or 'E' then maybe read the exponent (or backtrack) */
    if (idx < end_idx && (str[idx] == 'e' || str[idx] == 'E')) {
        Py_ssize_t e_start = idx;
        idx++;

        /* read an exponent sign if present */
        if (idx < end_idx && (str[idx] == '-' || str[idx] == '+')) idx++;

        /* read all digits */
        while (idx <= end_idx && str[idx] >= '0' && str[idx] <= '9') idx++;

        /* if we got a digit, then parse as float. if not, backtrack */
        if (str[idx - 1] >= '0' && str[idx - 1] <= '9') {
            is_float = 1;
        }
        else {
            idx = e_start;
        }
    }

    /* copy the section we determined to be a number */
    numstr = PyUnicode_FromUnicode(&str[start], idx - start);
    if (numstr == NULL)
        return NULL;
    if (is_float) {
        /* parse as a float using a fast path if available, otherwise call user defined method */
        if (s->parse_float != (PyObject *)&PyFloat_Type) {
            rval = PyObject_CallFunctionObjArgs(s->parse_float, numstr, NULL);
        }
        else {
            rval = PyFloat_FromString(numstr, NULL);
        }
    }
    else {
        /* no fast path for unicode -> int, just call */
        rval = PyObject_CallFunctionObjArgs(s->parse_int, numstr, NULL);
    }
    Py_DECREF(numstr);
    return _build_rval_index_tuple(rval, idx);
}

static PyObject *
scan_once_str(PyScannerObject *s, PyObject *pystr, Py_ssize_t idx)
{
    char *str = PyString_AS_STRING(pystr);
    Py_ssize_t length = PyString_GET_SIZE(pystr);
    Py_ssize_t next_idx = -1;
    PyObject *rval;
    if (idx >= length) {
        PyErr_SetNone(PyExc_StopIteration);
        return NULL;
    }
    switch (str[idx]) {
        case '"':
            /* string */
            rval = scanstring_str(pystr, idx + 1,
                PyString_AS_STRING(s->encoding),
                PyObject_IsTrue(s->strict),
                &next_idx);
            return _build_rval_index_tuple(rval, next_idx);
        case '{':
            /* object */
            return _parse_object_str(s, pystr, idx + 1);
        case '[':
            /* array */
            return _parse_array_str(s, pystr, idx + 1);
        case 'n':
            /* null */
            if ((idx + 3 < length) && str[idx + 1] == 'u' && str[idx + 2] == 'l' && str[idx + 3] == 'l') {
                Py_INCREF(Py_None);
                return _build_rval_index_tuple(Py_None, idx + 4);
            }
            break;
        case 't':
            /* true */
            if ((idx + 3 < length) && str[idx + 1] == 'r' && str[idx + 2] == 'u' && str[idx + 3] == 'e') {
                Py_INCREF(Py_True);
                return _build_rval_index_tuple(Py_True, idx + 4);
            }
            break;
        case 'f':
            /* false */
            if ((idx + 4 < length) && str[idx + 1] == 'a' && str[idx + 2] == 'l' && str[idx + 3] == 's' && str[idx + 4] == 'e') {
                Py_INCREF(Py_False);
                return _build_rval_index_tuple(Py_False, idx + 5);
            }
            break;
        case 'N':
            /* NaN */
            if ((idx + 2 < length) && str[idx + 1] == 'a' && str[idx + 2] == 'N') {
                return _parse_constant(s, "NaN", idx);
            }
            break;
        case 'I':
            /* Infinity */
            if ((idx + 7 < length) && str[idx + 1] == 'n' && str[idx + 2] == 'f' && str[idx + 3] == 'i' && str[idx + 4] == 'n' && str[idx + 5] == 'i' && str[idx + 6] == 't' && str[idx + 7] == 'y') {
                return _parse_constant(s, "Infinity", idx);
            }
            break;
        case '-':
            /* -Infinity */
            if ((idx + 8 < length) && str[idx + 1] == 'I' && str[idx + 2] == 'n' && str[idx + 3] == 'f' && str[idx + 4] == 'i' && str[idx + 5] == 'n' && str[idx + 6] == 'i' && str[idx + 7] == 't' && str[idx + 8] == 'y') {
                return _parse_constant(s, "-Infinity", idx);
            }
            break;
    }
    /* Didn't find a string, object, array, or named constant. Look for a number. */
    return _match_number_str(s, pystr, idx);
}

static PyObject *
scan_once_unicode(PyScannerObject *s, PyObject *pystr, Py_ssize_t idx)
{
    Py_UNICODE *str = PyUnicode_AS_UNICODE(pystr);
    Py_ssize_t length = PyUnicode_GET_SIZE(pystr);
    Py_ssize_t next_idx = -1;
    PyObject *rval;
    if (idx >= length) {
        PyErr_SetNone(PyExc_StopIteration);
        return NULL;
    }
    switch (str[idx]) {
        case '"':
            /* string */
            rval = scanstring_unicode(pystr, idx + 1,
                PyObject_IsTrue(s->strict),
                &next_idx);
            return _build_rval_index_tuple(rval, next_idx);
        case '{':
            /* object */
            return _parse_object_unicode(s, pystr, idx + 1);
        case '[':
            /* array */
            return _parse_array_unicode(s, pystr, idx + 1);
        case 'n':
            /* null */
            if ((idx + 3 < length) && str[idx + 1] == 'u' && str[idx + 2] == 'l' && str[idx + 3] == 'l') {
                Py_INCREF(Py_None);
                return _build_rval_index_tuple(Py_None, idx + 4);
            }
            break;
        case 't':
            /* true */
            if ((idx + 3 < length) && str[idx + 1] == 'r' && str[idx + 2] == 'u' && str[idx + 3] == 'e') {
                Py_INCREF(Py_True);
                return _build_rval_index_tuple(Py_True, idx + 4);
            }
            break;
        case 'f':
            /* false */
            if ((idx + 4 < length) && str[idx + 1] == 'a' && str[idx + 2] == 'l' && str[idx + 3] == 's' && str[idx + 4] == 'e') {
                Py_INCREF(Py_False);
                return _build_rval_index_tuple(Py_False, idx + 5);
            }
            break;
        case 'N':
            /* NaN */
            if ((idx + 2 < length) && str[idx + 1] == 'a' && str[idx + 2] == 'N') {
                return _parse_constant(s, "NaN", idx);
            }
            break;
        case 'I':
            /* Infinity */
            if ((idx + 7 < length) && str[idx + 1] == 'n' && str[idx + 2] == 'f' && str[idx + 3] == 'i' && str[idx + 4] == 'n' && str[idx + 5] == 'i' && str[idx + 6] == 't' && str[idx + 7] == 'y') {
                return _parse_constant(s, "Infinity", idx);
            }
            break;
        case '-':
            /* -Infinity */
            if ((idx + 8 < length) && str[idx + 1] == 'I' && str[idx + 2] == 'n' && str[idx + 3] == 'f' && str[idx + 4] == 'i' && str[idx + 5] == 'n' && str[idx + 6] == 'i' && str[idx + 7] == 't' && str[idx + 8] == 'y') {
                return _parse_constant(s, "-Infinity", idx);
            }
            break;
    }
    /* Didn't find a string, object, array, or named constant. Look for a number. */
    return _match_number_unicode(s, pystr, idx);
}

static PyObject *
scanner_call(PyObject *self, PyObject *args, PyObject *kwds)
{
    PyObject *pystr;
    Py_ssize_t idx;
    static char *kwlist[] = {"string", "idx", NULL};
    PyScannerObject *s = (PyScannerObject *)self;
    assert(PyScanner_Check(self));
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO&:scan_once", kwlist, &pystr, _convertPyInt_AsSsize_t, &idx))
        return NULL;
    if (PyString_Check(pystr)) {
        return scan_once_str(s, pystr, idx);
    }
    else if (PyUnicode_Check(pystr)) {
        return scan_once_unicode(s, pystr, idx);
    }
    else {
        PyErr_Format(PyExc_TypeError,
                 "first argument must be a string, not %.80s",
                 Py_TYPE(pystr)->tp_name);
        return NULL;
    }
}

static int
scanner_init(PyObject *self, PyObject *args, PyObject *kwds)
{
    PyObject *ctx;
    static char *kwlist[] = {"context", NULL};

    assert(PyScanner_Check(self));
    PyScannerObject *s = (PyScannerObject *)self;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O:make_scanner", kwlist, &ctx))
        return -1;

    s->encoding = NULL;
    s->strict = NULL;
    s->object_hook = NULL;
    s->parse_float = NULL;
    s->parse_int = NULL;
    s->parse_constant = NULL;

    /* PyString_AS_STRING is used on encoding */
    s->encoding = PyObject_GetAttrString(ctx, "encoding");
    if (s->encoding == Py_None) {
        Py_DECREF(Py_None);
        s->encoding = PyString_InternFromString(DEFAULT_ENCODING);
    }
    else if (PyUnicode_Check(s->encoding)) {
        PyObject *tmp = PyUnicode_AsEncodedString(s->encoding, NULL, NULL);
        Py_DECREF(s->encoding);
        s->encoding = tmp;
    }
    if (s->encoding == NULL || !PyString_Check(s->encoding))
        goto bail;
    
    /* All of these will fail "gracefully" so we don't need to verify them */
    s->strict = PyObject_GetAttrString(ctx, "strict");
    if (s->strict == NULL)
        goto bail;
    s->object_hook = PyObject_GetAttrString(ctx, "object_hook");
    if (s->object_hook == NULL)
        goto bail;
    s->parse_float = PyObject_GetAttrString(ctx, "parse_float");
    if (s->parse_float == NULL)
        goto bail;
    s->parse_int = PyObject_GetAttrString(ctx, "parse_int");
    if (s->parse_int == NULL)
        goto bail;
    s->parse_constant = PyObject_GetAttrString(ctx, "parse_constant");
    if (s->parse_constant == NULL)
        goto bail;
    
    return 0;

bail:
    Py_XDECREF(s->encoding);
    Py_XDECREF(s->strict);
    Py_XDECREF(s->object_hook);
    Py_XDECREF(s->parse_float);
    Py_XDECREF(s->parse_int);
    Py_XDECREF(s->parse_constant);
    s->encoding = NULL;
    s->strict = NULL;
    s->object_hook = NULL;
    s->parse_float = NULL;
    s->parse_int = NULL;
    s->parse_constant = NULL;
    return -1;
}

PyDoc_STRVAR(scanner_doc, "JSON scanner object");

static
PyTypeObject PyScannerType = {
    PyObject_HEAD_INIT(0)
    0,                    /* tp_internal */
    "make_scanner",       /* tp_name */
    sizeof(PyScannerObject), /* tp_basicsize */
    0,                    /* tp_itemsize */
    scanner_dealloc, /* tp_dealloc */
    0,                    /* tp_print */
    0,                    /* tp_getattr */
    0,                    /* tp_setattr */
    0,                    /* tp_compare */
    0,                    /* tp_repr */
    0,                    /* tp_as_number */
    0,                    /* tp_as_sequence */
    0,                    /* tp_as_mapping */
    0,                    /* tp_hash */
    scanner_call,         /* tp_call */
    0,                    /* tp_str */
    0,/* PyObject_GenericGetAttr, */                    /* tp_getattro */
    0,/* PyObject_GenericSetAttr, */                    /* tp_setattro */
    0,                    /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,   /* tp_flags */
    scanner_doc,          /* tp_doc */
    0,                    /* tp_traverse */
    0,                    /* tp_clear */
    0,                    /* tp_richcompare */
    0,                    /* tp_weaklistoffset */
    0,                    /* tp_iter */
    0,                    /* tp_iternext */
    0,                    /* tp_methods */
    scanner_members,                    /* tp_members */
    0,                    /* tp_getset */
    0,                    /* tp_base */
    0,                    /* tp_dict */
    0,                    /* tp_descr_get */
    0,                    /* tp_descr_set */
    0,                    /* tp_dictoffset */
    scanner_init,                    /* tp_init */
    0,/* PyType_GenericAlloc, */        /* tp_alloc */
    0,/* PyType_GenericNew, */          /* tp_new */
    0,/* _PyObject_Del, */              /* tp_free */
};

static int
encoder_init(PyObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"markers", "default", "encoder", "indent", "key_separator", "item_separator", "sort_keys", "skipkeys", "allow_nan", NULL};

    assert(PyEncoder_Check(self));
    PyEncoderObject *s = (PyEncoderObject *)self;
    PyObject *allow_nan;

    s->markers = NULL;
    s->defaultfn = NULL;
    s->encoder = NULL;
    s->indent = NULL;
    s->key_separator = NULL;
    s->item_separator = NULL;
    s->sort_keys = NULL;
    s->skipkeys = NULL;
    
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OOOOOOOOO:make_encoder", kwlist,
        &s->markers, &s->defaultfn, &s->encoder, &s->indent, &s->key_separator, &s->item_separator, &s->sort_keys, &s->skipkeys, &allow_nan))
        return -1;
    
    Py_INCREF(s->markers);
    Py_INCREF(s->defaultfn);
    Py_INCREF(s->encoder);
    Py_INCREF(s->indent);
    Py_INCREF(s->key_separator);
    Py_INCREF(s->item_separator);
    Py_INCREF(s->sort_keys);
    Py_INCREF(s->skipkeys);
    s->fast_encode = (PyCFunction_Check(s->encoder) && PyCFunction_GetFunction(s->encoder) == (PyCFunction)py_encode_basestring_ascii);
    s->allow_nan = PyObject_IsTrue(allow_nan);
    return 0;
}

static PyObject *
encoder_call(PyObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"obj", "_current_indent_level", NULL};
    PyObject *obj;
    PyObject *rval;
    Py_ssize_t indent_level;
    PyEncoderObject *s = (PyEncoderObject *)self;
    assert(PyEncoder_Check(self));
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO&:_iterencode", kwlist,
        &obj, _convertPyInt_AsSsize_t, &indent_level))
        return NULL;
    rval = PyList_New(0);
    if (rval == NULL)
        return NULL;
    if (encoder_listencode_obj(s, rval, obj, indent_level)) {
        Py_DECREF(rval);
        return NULL;
    }
    return rval;
}

static PyObject *
_encoded_const(PyObject *obj)
{
    if (obj == Py_None) {
        static PyObject *s_null = NULL;
        if (s_null == NULL) {
            s_null = PyString_InternFromString("null");
        }
        return s_null;
    }
    else if (obj == Py_True) {
        static PyObject *s_true = NULL;
        if (s_true == NULL) {
            s_true = PyString_InternFromString("true");
        }
        return s_true;
    }
    else if (obj == Py_False) {
        static PyObject *s_false = NULL;
        if (s_false == NULL) {
            s_false = PyString_InternFromString("false");
        }
        return s_false;
    }
    else {
        PyErr_SetString(PyExc_ValueError, "not a const");
        return NULL;
    }
}

static PyObject *
encoder_encode_float(PyEncoderObject *s, PyObject *obj)
{
    double i = PyFloat_AS_DOUBLE(obj);
    if (!Py_IS_FINITE(i)) {
        if (!s->allow_nan) {
            PyErr_SetString(PyExc_ValueError, "Out of range float values are not JSON compliant");
            return NULL;
        }
        if (i > 0) {
            return PyString_FromString("Infinity");
        }
        else if (i < 0) {
            return PyString_FromString("-Infinity");
        }
        else {
            return PyString_FromString("NaN");
        }
    }
    /* Use a better float format here? */
    return PyObject_Repr(obj);
}

static PyObject *
encoder_encode_string(PyEncoderObject *s, PyObject *obj)
{
    if (s->fast_encode)
        return py_encode_basestring_ascii(NULL, obj);
    else
        return PyObject_CallFunctionObjArgs(s->encoder, obj, NULL);
}

static int
encoder_listencode_obj(PyEncoderObject *s, PyObject *rval, PyObject *obj, Py_ssize_t indent_level)
{
    if (obj == Py_None || obj == Py_True || obj == Py_False) {
        PyObject *cstr = _encoded_const(obj);
        if (cstr == NULL)
            return -1;
        return PyList_Append(rval, cstr);
    }
    else if (PyString_Check(obj) || PyUnicode_Check(obj))
    {
        PyObject *encoded = encoder_encode_string(s, obj);
        if (encoded == NULL)
            return -1;
        return PyList_Append(rval, encoded);
    }
    else if (PyInt_Check(obj) || PyLong_Check(obj)) {
        PyObject *encoded = PyObject_Str(obj);
        if (encoded == NULL)
            return -1;
        return PyList_Append(rval, encoded);
    }
    else if (PyFloat_Check(obj)) {
        PyObject *encoded = encoder_encode_float(s, obj);
        if (encoded == NULL)
            return -1;
        return PyList_Append(rval, encoded);
    }
    else if (PyList_Check(obj) || PyTuple_Check(obj)) {
        return encoder_listencode_list(s, rval, obj, indent_level);
    }
    else if (PyDict_Check(obj)) {
        return encoder_listencode_dict(s, rval, obj, indent_level);
    }
    else {
        PyObject *ident = NULL;
        if (s->markers != Py_None) {
            ident = PyLong_FromVoidPtr(obj);
            int has_key;
            if (ident == NULL)
                return -1;
            has_key = PyDict_Contains(s->markers, ident);
            if (has_key) {
                if (has_key != -1)
                    PyErr_SetString(PyExc_ValueError, "Circular reference detected");
                Py_DECREF(ident);
                return -1;
            }
            if (PyDict_SetItem(s->markers, ident, obj)) {
                Py_DECREF(ident);
                return -1;
            }
        }
        PyObject *newobj = PyObject_CallFunctionObjArgs(s->defaultfn, obj, NULL);
        if (newobj == NULL) {
            Py_DECREF(ident);
            return -1;
        }
        int rv = encoder_listencode_obj(s, rval, newobj, indent_level);
        Py_DECREF(newobj);
        if (rv) {
            Py_DECREF(ident);
            return -1;
        }
        if (ident != NULL) {
            if (PyDict_DelItem(s->markers, ident)) {
                Py_DECREF(ident);
                ident = NULL;
                return -1;
            }
            Py_DECREF(ident);
            ident = NULL;
        }
        return rv;
    }
}

static int
encoder_listencode_dict(PyEncoderObject *s, PyObject *rval, PyObject *dct, Py_ssize_t indent_level)
{
    static PyObject *open_dict = NULL;
    static PyObject *close_dict = NULL;
    static PyObject *empty_dict = NULL;
    PyObject *kstr = NULL;
    if (open_dict == NULL || close_dict == NULL || empty_dict == NULL) {
        open_dict = PyString_InternFromString("{");
        close_dict = PyString_InternFromString("}");
        empty_dict = PyString_InternFromString("{}");
        if (open_dict == NULL || close_dict == NULL || empty_dict == NULL)
            return -1;
    }
    PyObject *ident = NULL;
    if (PyDict_Size(dct) == 0)
        return PyList_Append(rval, empty_dict);
    
    if (s->markers != Py_None) {
        ident = PyLong_FromVoidPtr(dct);
        int has_key;
        if (ident == NULL)
            goto bail;
        has_key = PyDict_Contains(s->markers, ident);
        if (has_key) {
            if (has_key != -1)
                PyErr_SetString(PyExc_ValueError, "Circular reference detected");
            goto bail;
        }
        if (PyDict_SetItem(s->markers, ident, dct)) {
            goto bail;
        }
    }

    if (PyList_Append(rval, open_dict))
        goto bail;

    if (s->indent != Py_None) {
        /* TODO: DOES NOT RUN */
        indent_level += 1;
        /*
            newline_indent = '\n' + (' ' * (_indent * _current_indent_level))
            separator = _item_separator + newline_indent
            buf += newline_indent
        */
    }

    /* TODO: C speedup not implemented for sort_keys */

    PyObject *key, *value;
    Py_ssize_t pos = 0;
    int skipkeys = PyObject_IsTrue(s->skipkeys);
    Py_ssize_t idx = 0;
    while (PyDict_Next(dct, &pos, &key, &value)) {
        if (PyString_Check(key) || PyUnicode_Check(key)) {
            Py_INCREF(key);
            kstr = key;
        }
        else if (PyFloat_Check(key)) {
            kstr = encoder_encode_float(s, key);
            if (kstr == NULL)
                goto bail;
        }
        else if (PyInt_Check(key) || PyLong_Check(key)) {
            kstr = PyObject_Str(key);
            if (kstr == NULL)
                goto bail;
        }
        else if (key == Py_True || key == Py_False || key == Py_None) {
            kstr = _encoded_const(key);
        }
        else if (skipkeys) {
            continue;
        }
        else {
            /* TODO: include repr of key */
            PyErr_SetString(PyExc_ValueError, "keys must be a string");
            goto bail;
        }
        
        if (idx) {
            if (PyList_Append(rval, s->item_separator))
                goto bail;
        }
        
        PyObject *encoded = encoder_encode_string(s, kstr);
        Py_DECREF(kstr);
        kstr = NULL;
        if (encoded == NULL)
            goto bail;
        if (PyList_Append(rval, encoded))
            goto bail;
        if (PyList_Append(rval, s->key_separator))
            goto bail;
        if (encoder_listencode_obj(s, rval, value, indent_level))
            goto bail;
        idx += 1;
    }
    if (ident != NULL) {
        if (PyDict_DelItem(s->markers, ident))
            goto bail;
        Py_DECREF(ident);
        ident = NULL;
    }
    if (s->indent != Py_None) {
        /* TODO: DOES NOT RUN */
        indent_level -= 1;
        /*
            yield '\n' + (' ' * (_indent * _current_indent_level))
        */
    }
    if (PyList_Append(rval, close_dict))
        goto bail;
    return 0;
    
bail:
    Py_XDECREF(kstr);
    Py_XDECREF(ident);
    return -1;
}


static int
encoder_listencode_list(PyEncoderObject *s, PyObject *rval, PyObject *seq, Py_ssize_t indent_level)
{
    static PyObject *open_array = NULL;
    static PyObject *close_array = NULL;
    static PyObject *empty_array = NULL;
    if (open_array == NULL || close_array == NULL || empty_array == NULL) {
        open_array = PyString_InternFromString("[");
        close_array = PyString_InternFromString("]");
        empty_array = PyString_InternFromString("[]");
        if (open_array == NULL || close_array == NULL || empty_array == NULL)
            return -1;
    }
    PyObject *ident = NULL;
    PyObject *s_fast = PySequence_Fast(seq, "_iterencode_list needs a sequence");
    if (s_fast == NULL)
        return -1;
    Py_ssize_t num_items = PySequence_Fast_GET_SIZE(s_fast);
    if (num_items == 0) {
        Py_DECREF(s_fast);
        return PyList_Append(rval, empty_array);
    }
    
    if (s->markers != Py_None) {
        ident = PyLong_FromVoidPtr(seq);
        int has_key;
        if (ident == NULL)
            goto bail;
        has_key = PyDict_Contains(s->markers, ident);
        if (has_key) {
            if (has_key != -1)
                PyErr_SetString(PyExc_ValueError, "Circular reference detected");
            goto bail;
        }
        if (PyDict_SetItem(s->markers, ident, seq)) {
            goto bail;
        }
    }
    
    PyObject **seq_items = PySequence_Fast_ITEMS(s_fast);
    if (PyList_Append(rval, open_array))
        goto bail;
    Py_ssize_t i;
    if (s->indent != Py_None) {
        /* TODO: DOES NOT RUN */
        indent_level += 1;
        /*
            newline_indent = '\n' + (' ' * (_indent * _current_indent_level))
            separator = _item_separator + newline_indent
            buf += newline_indent
        */
    }
    for (i = 0; i < num_items; i++) {
        PyObject *obj = seq_items[i];
        if (i) {
            if (PyList_Append(rval, s->item_separator))
                goto bail;
        }
        if (encoder_listencode_obj(s, rval, obj, indent_level))
            goto bail;
    }
    if (ident != NULL) {
        if (PyDict_DelItem(s->markers, ident))
            goto bail;
        Py_DECREF(ident);
        ident = NULL;
    }
    if (s->indent != Py_None) {
        /* TODO: DOES NOT RUN */
        indent_level -= 1;
        /*
            yield '\n' + (' ' * (_indent * _current_indent_level))
        */
    }
    if (PyList_Append(rval, close_array))
        goto bail;
    Py_DECREF(s_fast);
    return 0;
    
bail:
    Py_XDECREF(ident);
    Py_DECREF(s_fast);
    return -1;
}

static void
encoder_dealloc(PyObject *self)
{
    assert(PyEncoder_Check(self));
    PyEncoderObject *s = (PyEncoderObject *)self;
    Py_XDECREF(s->markers);
    s->markers = NULL;
    Py_XDECREF(s->defaultfn);
    s->defaultfn = NULL;
    Py_XDECREF(s->encoder);
    s->encoder = NULL;
    Py_XDECREF(s->indent);
    s->indent = NULL;
    Py_XDECREF(s->key_separator);
    s->key_separator = NULL;
    Py_XDECREF(s->item_separator);
    s->item_separator = NULL;
    Py_XDECREF(s->sort_keys);
    s->sort_keys = NULL;
    Py_XDECREF(s->skipkeys);
    s->skipkeys = NULL;
    self->ob_type->tp_free(self);
}

PyDoc_STRVAR(encoder_doc, "_iterencode(obj, _current_indent_level) -> iterable");

static
PyTypeObject PyEncoderType = {
    PyObject_HEAD_INIT(0)
    0,                    /* tp_internal */
    "make_encoder",       /* tp_name */
    sizeof(PyEncoderObject), /* tp_basicsize */
    0,                    /* tp_itemsize */
    encoder_dealloc, /* tp_dealloc */
    0,                    /* tp_print */
    0,                    /* tp_getattr */
    0,                    /* tp_setattr */
    0,                    /* tp_compare */
    0,                    /* tp_repr */
    0,                    /* tp_as_number */
    0,                    /* tp_as_sequence */
    0,                    /* tp_as_mapping */
    0,                    /* tp_hash */
    encoder_call,                    /* tp_call */
    0,                    /* tp_str */
    0,/* PyObject_GenericGetAttr, */                    /* tp_getattro */
    0,/* PyObject_GenericSetAttr, */                    /* tp_setattro */
    0,                    /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,   /* tp_flags */
    encoder_doc,          /* tp_doc */
    0,                    /* tp_traverse */
    0,                    /* tp_clear */
    0,                    /* tp_richcompare */
    0,                    /* tp_weaklistoffset */
    0,                    /* tp_iter */
    0,                    /* tp_iternext */
    0,                    /* tp_methods */
    encoder_members,                    /* tp_members */
    0,                    /* tp_getset */
    0,                    /* tp_base */
    0,                    /* tp_dict */
    0,                    /* tp_descr_get */
    0,                    /* tp_descr_set */
    0,                    /* tp_dictoffset */
    encoder_init,                    /* tp_init */
    0,/* PyType_GenericAlloc, */       /* tp_alloc */
    0,/* PyType_GenericNew, */         /* tp_new */
    0,/* _PyObject_Del, */             /* tp_free */
};

static PyMethodDef speedups_methods[] = {
    {"encode_basestring_ascii",
        (PyCFunction)py_encode_basestring_ascii,
        METH_O,
        pydoc_encode_basestring_ascii},
    {"scanstring",
        (PyCFunction)py_scanstring,
        METH_VARARGS,
        pydoc_scanstring},
    {NULL, NULL, 0, NULL}
};

PyDoc_STRVAR(module_doc,
"simplejson speedups\n");

void
init_speedups(void)
{
    PyObject *m;
    PyScannerType.tp_getattro = PyObject_GenericGetAttr;
    PyScannerType.tp_setattro = PyObject_GenericSetAttr;
    PyScannerType.tp_alloc  = PyType_GenericAlloc;
    PyScannerType.tp_new = PyType_GenericNew;
    PyScannerType.tp_free = _PyObject_Del;
    if (PyType_Ready(&PyScannerType) < 0)
        return;
    PyEncoderType.tp_getattro = PyObject_GenericGetAttr;
    PyEncoderType.tp_setattro = PyObject_GenericSetAttr;
    PyEncoderType.tp_alloc  = PyType_GenericAlloc;
    PyEncoderType.tp_new = PyType_GenericNew;
    PyEncoderType.tp_free = _PyObject_Del;
    if (PyType_Ready(&PyEncoderType) < 0)
        return;
    m = Py_InitModule3("_speedups", speedups_methods, module_doc);
    Py_INCREF((PyObject*)&PyScannerType);
    PyModule_AddObject(m, "make_scanner", (PyObject*)&PyScannerType);
    Py_INCREF((PyObject*)&PyEncoderType);
    PyModule_AddObject(m, "make_encoder", (PyObject*)&PyEncoderType);
}
