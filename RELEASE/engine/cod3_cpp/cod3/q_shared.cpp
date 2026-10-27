// Call of Duty 3's q_shared: strings and info strings, as the original source
// would read. [acct2-decomp]
//
// Written from the recompiled C++ (recomp/generated: the DEFINE_REX_FUNC named
// at each function in q_shared.h, read on the owner's PC with gen_tool.py) and
// checked against the original machine code; the Rust ports
// (engine/crates/cod3_game/src/shared/) give the same results. Not from Quake
// III's source (GPL; names and understanding only). Local names are ours.
//
// Where CoD3 differs from its Quake III ancestor it says so; those are the
// console's behavior, kept on purpose.
#include <cstdarg>
#include <cstring>

#include "cod3/q_shared.h"
#include "cod3/xbox_crt.h"

namespace cod3 {

namespace {

// A character as the console reads it: lbz then extsb, a signed char.
inline int Char(const char* s) { return static_cast<signed char>(*s); }

}  // namespace

// ---- Com_sprintf ---------------------------------------------------------------

void Com_sprintf(char* dest, int size, const char* fmt, ...) {
    std::va_list args;
    va_start(args, fmt);
    xbox_crt::_vsnprintf(dest, static_cast<std::size_t>(size), fmt, args);
    va_end(args);
    dest[size - 1] = '\0';  // in every case, even after a short text
}

// ---- comparing -------------------------------------------------------------------

int Q_stricmpn(const char* s1, const char* s2, int n) {
    int c1;
    int c2;
    do {
        c1 = Char(s1++);
        c2 = Char(s2++);
        if (!n--) {
            return 0;  // the first n characters are equal
        }
        if (c1 != c2) {
            if (c1 >= 'A' && c1 <= 'Z') {
                c1 += 'a' - 'A';
            }
            if (c2 >= 'A' && c2 <= 'Z') {
                c2 += 'a' - 'A';
            }
            if (c1 != c2) {
                return c1 - c2;
            }
        }
    } while (c1);
    return 0;
}

int Q_stricmp(const char* s1, const char* s2) {
    // Q_stricmpn(s1, s2, 99999) inlined, as in Quake III, but changed: the
    // fold is to upper case and a difference is -1 whatever its sign.
    int n = 99999;
    int c1;
    int c2;
    do {
        c1 = Char(s1++);
        c2 = Char(s2++);
        if (!n--) {
            return 0;
        }
        if (c1 != c2) {
            if (c1 >= 'a' && c1 <= 'z') {
                c1 -= 'a' - 'A';
            }
            if (c2 >= 'a' && c2 <= 'z') {
                c2 -= 'a' - 'A';
            }
            if (c1 != c2) {
                return -1;
            }
        }
    } while (c1);
    return 0;
}

// ---- changing ----------------------------------------------------------------------

char* Q_strlwr(char* s1) {
    for (char* s = s1; *s; ++s) {
        *s = static_cast<char>(xbox_crt::tolower(Char(s)));
    }
    return s1;
}

void Q_strcat(char* dest, int size, const char* src) {
    const int length = static_cast<int>(std::strlen(dest));
    if (length >= size) {
        Com_Error(ERR_FATAL, "Q_strcat: already overflowed");
    }
    // Q_strncpyz(dest + length, src, size - length), inlined: strncpy pads the
    // rest of the buffer with zeros, and the last byte is cleared.
    std::strncpy(dest + length, src, static_cast<std::size_t>(size - length - 1));
    dest[size - 1] = '\0';
}

// ---- file names ----------------------------------------------------------------------

const char* COM_GetExtension(const char* name) {
    const char* extension = "";
    for (const char* c = name; *c; ++c) {
        if (*c == '.') {
            extension = c + 1;
        } else if (*c == '/' || *c == '\\') {
            extension = "";  // a dot in a folder's name doesn't count
        }
    }
    return extension;
}

void COM_DefaultExtension(char* path, int maxSize, const char* extension) {
    char oldPath[128];
    // Looking back from the last character to the last '/' (not '\'), a '.'
    // means the name already has an extension. The first character isn't
    // looked at, so ".cfg" alone gets another one ("..cfg").
    const std::size_t length = std::strlen(path);
    if (length > 0) {  // (an empty name makes the original read path[-1]; here it just gets the extension)
        const char* src = path + length - 1;
        while (*src != '/' && src != path) {
            if (*src == '.') {
                return;
            }
            --src;
        }
    }
    // Q_strncpyz(oldPath, path, sizeof(oldPath)), inlined.
    std::strncpy(oldPath, path, sizeof(oldPath) - 1);
    oldPath[sizeof(oldPath) - 1] = '\0';
    Com_sprintf(path, maxSize, "%s%s", oldPath, extension);
}

// ---- info strings -------------------------------------------------------------------------

const char* Info_ValueForKey(const char* s, const char* key) {
    char keyText[BIG_INFO_KEY];
    // Two buffers used in turn (0x829FBFA0, 1024 bytes each; the index at
    // 0x82AB1BE0), so a caller can hold one value while asking for another.
    static char values[2][MAX_INFO_VALUE];
    static int valueIndex = 0;

    if (!s || !key) {
        return "";
    }
    if (std::strlen(s) >= MAX_INFO_STRING) {
        Com_Error(ERR_DROP, "Info_ValueForKey: oversize infostring");
    }
    valueIndex ^= 1;
    if (*s == '\\') {
        s++;
    }
    while (true) {
        char* o = keyText;
        while (*s != '\\') {
            if (!*s) {
                return "";  // a key without a value ends the string
            }
            *o++ = *s++;
        }
        *o = '\0';
        s++;

        o = values[valueIndex];
        while (*s != '\\' && *s) {
            *o++ = *s++;
        }
        *o = '\0';

        if (!Q_stricmpn(key, keyText, 99999)) {
            return values[valueIndex];
        }
        if (!*s) {
            return "";
        }
        s++;
    }
}

namespace {

// Info_RemoveKey and Info_RemoveKey_Big: one source, two copies in the
// executable (only their oversize messages differ).
void RemoveKey(char* s, const char* key, const char* oversizeMessage) {
    char keyText[MAX_INFO_KEY];
    char valueText[MAX_INFO_VALUE];

    if (std::strlen(s) >= MAX_INFO_STRING) {
        Com_Error(ERR_DROP, "%s", oversizeMessage);
    }
    if (std::strchr(key, '\\')) {
        return;
    }
    while (true) {
        char* start = s;
        if (*s == '\\') {
            s++;
        }
        char* o = keyText;
        while (*s != '\\') {
            if (!*s) {
                return;
            }
            *o++ = *s++;
        }
        *o = '\0';
        s++;

        o = valueText;
        while (*s != '\\' && *s) {
            *o++ = *s++;
        }
        *o = '\0';

        if (!std::strcmp(key, keyText)) {  // exact case, unlike Info_ValueForKey
            // strcpy(start, s): the rest of the string moves down over the pair.
            while ((*start++ = *s++) != '\0') {
            }
            return;
        }
        if (!*s) {
            return;
        }
    }
}

// The value as stored: its first 1023 characters, without '\', ';' and '"'.
void CleanValue(char* cleanValue, const char* value) {
    int length = 0;
    for (int i = 0; i < MAX_INFO_VALUE - 1 && value[i]; ++i) {
        if (value[i] != '\\' && value[i] != ';' && value[i] != '"') {
            cleanValue[length++] = value[i];
        }
    }
    cleanValue[length] = '\0';
}

// A key with '\', ';' or '"' is refused (after the value was cleaned, as in the original).
void CheckKey(const char* key, const char* value) {
    if (std::strchr(key, '\\')) {
        Com_Error(ERR_DROP, "Can't use keys with a \\\nkey: '%s'\nvalue: '%s'", key, value);
    }
    if (std::strchr(key, ';')) {
        Com_Error(ERR_DROP, "Can't use keys with a semicolon\nkey: '%s'\nvalue: '%s'", key, value);
    }
    if (std::strchr(key, '"')) {
        Com_Error(ERR_DROP, "Can't use keys with a \"\nkey: '%s'\nvalue: '%s'", key, value);
    }
}

}  // namespace

void Info_RemoveKey(char* s, const char* key) { RemoveKey(s, key, "Info_RemoveKey: oversize infostring"); }

void Info_RemoveKey_Big(char* s, const char* key) { RemoveKey(s, key, "Info_RemoveKey_Big: oversize infostring"); }

void Info_SetValueForKey(char* s, const char* key, const char* value) {
    char newPair[MAX_INFO_STRING];
    char cleanValue[MAX_INFO_VALUE];

    CleanValue(cleanValue, value);
    CheckKey(key, value);
    Info_RemoveKey(s, key);
    if (!cleanValue[0]) {
        return;
    }
    Com_sprintf(newPair, sizeof(newPair), "\\%s\\%s", key, cleanValue);
    // No length check: CoD3 appends past a full info string (Quake III
    // printed "Info string length exceeded" and stopped).
    std::strcat(s, newPair);
}

void Info_SetValueForKey_Big(char* s, const char* key, const char* value) {
    char newPair[MAX_INFO_STRING];
    char cleanValue[MAX_INFO_VALUE];

    if (std::strlen(s) >= MAX_INFO_STRING) {
        Com_Error(ERR_DROP, "Info_SetValueForKey: oversize infostring");
    }
    CleanValue(cleanValue, value);
    CheckKey(key, value);
    Info_RemoveKey_Big(s, key);
    if (!cleanValue[0]) {
        return;
    }
    Com_sprintf(newPair, sizeof(newPair), "\\%s\\%s", key, cleanValue);
    if (std::strlen(newPair) + std::strlen(s) > MAX_INFO_STRING) {
        Com_Error(ERR_DROP, "BIG Info string length exceeded\nkey: '%s'\nvalue: '%s'\nInfo string:\n%s\n", key,
                  value, s);
    }
    std::strcat(s, newPair);
}

}  // namespace cod3
