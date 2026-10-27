// Unit tests that need no game files: the PowerPC rules, known answers from
// the Rust crate's tests, and the platform checks the exact math depends on.
// [acct2-decomp]
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include "cod3/q_shared.h"
#include "cod3/scr_math.h"
#include "cod3/tr_public.h"
#include "cod3/xbox_crt.h"
#include "fma_cases.h"

namespace {

int failures = 0;

void Check(bool ok, const char* what, int line) {
    if (!ok) {
        std::printf("FAILED line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

std::uint32_t Bits(float f) { return std::bit_cast<std::uint32_t>(f); }
std::uint64_t Bits(double d) { return std::bit_cast<std::uint64_t>(d); }

void ConversionsSaturateLikeThePowerPC() {
    using cod3::ppc::fctid;
    using cod3::ppc::fctidz;
    using cod3::ppc::fctiwz;
    CHECK(fctiwz(std::numeric_limits<double>::quiet_NaN()) == std::numeric_limits<std::int32_t>::min());
    CHECK(fctiwz(1e20) == std::numeric_limits<std::int32_t>::max());
    CHECK(fctiwz(-1e20) == std::numeric_limits<std::int32_t>::min());
    CHECK(fctiwz(3e9) == 0x7FFFFFFF);
    CHECK(fctiwz(-2.9) == -2);
    CHECK(fctiwz(-2147483648.9) == std::numeric_limits<std::int32_t>::min());
    CHECK(fctidz(std::numeric_limits<double>::quiet_NaN()) == std::numeric_limits<std::int64_t>::min());
    CHECK(fctid(2.5) == 2);
    CHECK(fctid(3.5) == 4);
    CHECK(fctid(-2.5) == -2);
    CHECK(fctid(1e300) == std::numeric_limits<std::int64_t>::max());
}

// fmadds must be exact on the hard cases, and rounding through double must be
// wrong on all of them (so the cases stay sharp).
void FusedMultiplyAddRoundsOnce() {
    for (const auto& c : cod3::tests::kF32Cases) {
        const float a = std::bit_cast<float>(c[0]);
        const float b = std::bit_cast<float>(c[1]);
        const float add = std::bit_cast<float>(c[2]);
        CHECK(Bits(cod3::ppc::fmadds(a, b, add)) == c[3]);
        const float rounded_twice = static_cast<float>(static_cast<double>(a) * static_cast<double>(b) +
                                                       static_cast<double>(add));
        CHECK(Bits(rounded_twice) != c[3]);
    }
}

// The C runtime's sin/cos port uses std::fma on doubles. If this fails, this
// platform's fma rounds twice (as some C libraries do, through the x87's
// 80-bit format), and players on it would compute other sin/cos bits.
void ThePlatformsDoubleFmaIsExact() {
    for (const auto& c : cod3::tests::kF64Cases) {
        const double a = std::bit_cast<double>(c[0]);
        const double b = std::bit_cast<double>(c[1]);
        const double add = std::bit_cast<double>(c[2]);
        CHECK(Bits(std::fma(a, b, add)) == c[3]);
    }
}

void AnglesInSixteenBitSteps() {
    CHECK(cod3::AngleNormalize180(190.0f) == -170.00244f);
    CHECK(cod3::AngleNormalize180(45.5f) == 45.499878f);
    CHECK(cod3::AngleSubtract(10.0f, 350.0f) == 20.0f);
    float out = 0.0f;
    const float in[] = {190.0f, -190.0f, -600.0f, 45.5f};
    const float expected[] = {-170.0f, -190.0f, -240.0f, 45.5f};  // below -180 stays below: fmod keeps the sign
    for (int k = 0; k < 4; ++k) {
        cod3::AngleWrap180(&out, &in[k]);
        CHECK(out == expected[k]);
    }
}

void Vectors() {
    cod3::vec3_t v = {3.0f, 0.0f, 4.0f};
    CHECK(cod3::VectorNormalize(v) == 5.0f);
    CHECK(v[0] == 0.6f && v[1] == 0.0f && v[2] == 0.8f);
    const cod3::vec3_t zero = {0.0f, 0.0f, 0.0f};
    cod3::vec3_t out = {1.0f, 1.0f, 1.0f};
    CHECK(cod3::VectorNormalize2(zero, out) == 0.0f);
    CHECK(out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f);
    cod3::vec3_t f = {3.0f, 0.0f, 4.0f};
    cod3::VectorNormalizeFast(f);
    CHECK(std::fabs(f[0] - 0.6f) < 0.002f);
    const cod3::vec3_t p1 = {1.0f, 2.0f, 3.0f};
    const cod3::vec3_t p2 = {4.0f, 6.0f, 3.0f};
    CHECK(cod3::Distance(p1, p2) == 5.0f);
}

void RuntimeSinCos() {
    for (int i = -1000; i < 1000; ++i) {
        const double x = static_cast<double>(i) * 0.0137;
        CHECK(std::fabs(cod3::xbox_crt::sin(x) - std::sin(x)) < 1e-15);
        CHECK(std::fabs(cod3::xbox_crt::cos(x) - std::cos(x)) < 1e-15);
    }
    CHECK(Bits(cod3::xbox_crt::sin(-0.0)) == Bits(-0.0));
    CHECK(cod3::xbox_crt::cos(0.0) == 1.0);
    CHECK(std::isnan(cod3::xbox_crt::sin(3e8)));
    CHECK(Bits(cod3::xbox_crt::sin(3e8)) == 0xFFF8000000000000u);
}

void FastYawIsCloseToAtan2() {
    for (int i = 0; i < 3600; ++i) {
        const float degrees = static_cast<float>(i) * 0.1f;
        const float radians = degrees * (3.14159265f / 180.0f);
        const cod3::vec3_t v = {std::cos(radians) * 37.0f, std::sin(radians) * 37.0f, 5.0f};
        const float yaw = cod3::vectoyaw(v);
        const float error = std::fmin(std::fabs(yaw - degrees), 360.0f - std::fabs(yaw - degrees));
        CHECK(error < 0.01f);
        const float signed_yaw = cod3::vectoyaw_signed(v);
        CHECK(signed_yaw >= -180.0f && signed_yaw <= 180.0f);
    }
    const cod3::vec3_t up = {0.0f, 0.0f, 9.0f};
    const cod3::vec3_t down_y = {0.0f, -1.0f, 0.0f};
    CHECK(cod3::vectoyaw(up) == 0.0f);
    CHECK(cod3::vectoyaw(down_y) == 270.0f);
    CHECK(cod3::vectoyaw_signed(down_y) == -90.0f);
}

void ScriptRandom() {
    cod3::brocRandomSeed = cod3::kBrocRandomInitialSeed;
    const int a = cod3::Broc_RandomInt(100);
    CHECK(a >= 0 && a < 100);
    cod3::brocRandomSeed = cod3::kBrocRandomInitialSeed;
    CHECK(cod3::Broc_RandomInt(100) == a);
    const int r = cod3::Broc_RandomIntRange(9, 5);  // reversed bounds count from max
    CHECK(r >= 5 && r < 9);
}

// The shared area's known answers (the Rust crate's tests, written again here).
void Strings() {
    CHECK(cod3::Q_stricmp("Hello", "hELLO") == 0);
    CHECK(cod3::Q_stricmp("abc", "abd") == -1);
    CHECK(cod3::Q_stricmp("abd", "abc") == -1);  // never positive
    CHECK(cod3::Q_stricmpn("abd", "ABC", 99) == 1);
    CHECK(cod3::Q_stricmpn("abc", "abd", 2) == 0);
    CHECK(cod3::Q_stricmpn("\x80", "", 5) == -128);  // a signed char
    CHECK(std::strcmp(cod3::COM_GetExtension("maps/a.b/c"), "") == 0);
    CHECK(std::strcmp(cod3::COM_GetExtension("x.tar.gz"), "gz") == 0);
    char path[128] = "auto";
    cod3::COM_DefaultExtension(path, 128, ".cfg");
    CHECK(std::strcmp(path, "auto.cfg") == 0);
    char has[128] = "a.txt";
    cod3::COM_DefaultExtension(has, 128, ".cfg");
    CHECK(std::strcmp(has, "a.txt") == 0);
    char dest[8] = {'a', 'b', 0, 'x', 'x', 'x', 'x', 'x'};
    cod3::Q_strcat(dest, 8, "cd");
    CHECK(std::memcmp(dest, "abcd\0\0\0\0", 8) == 0);  // padded with zeros
    char full[5] = {'a', 'b', 'c', 'd', 0};
    bool fatal = false;
    try {
        cod3::Q_strcat(full, 4, "x");
    } catch (const cod3::ComError& e) {
        fatal = e.code == cod3::ERR_FATAL;
    }
    CHECK(fatal);
}

void InfoStrings() {
    const char* info = "\\name\\Price\\Team\\allies";
    CHECK(std::strcmp(cod3::Info_ValueForKey(info, "team"), "allies") == 0);  // case ignored
    CHECK(std::strcmp(cod3::Info_ValueForKey(info, "rank"), "") == 0);
    CHECK(std::strcmp(cod3::Info_ValueForKey("a\\1", "A"), "1") == 0);
    // Two values held at once: the two static buffers.
    const char* name = cod3::Info_ValueForKey(info, "name");
    const char* team = cod3::Info_ValueForKey(info, "Team");
    CHECK(std::strcmp(name, "Price") == 0 && std::strcmp(team, "allies") == 0);
    char s[2048] = "\\a\\1\\b\\2";
    cod3::Info_SetValueForKey(s, "a", "x;y");
    CHECK(std::strcmp(s, "\\b\\2\\a\\xy") == 0);
    cod3::Info_RemoveKey(s, "b");
    CHECK(std::strcmp(s, "\\a\\xy") == 0);
    bool dropped = false;
    try {
        cod3::Info_SetValueForKey(s, "bad;key", "1");
    } catch (const cod3::ComError& e) {
        dropped = e.code == cod3::ERR_DROP;
    }
    CHECK(dropped);
}

void Hashes() {
    CHECK(cod3::NameHash("") == 0);
    CHECK(cod3::NameHash(nullptr) == 0);
    CHECK(cod3::NameHash("End") == cod3::NameHash("end"));
    CHECK(cod3::NameHash("ab") == 97u * 33u + 98u);
    cod3::nameKey_t key;
    cod3::NameHash_Init(&key, "a_very_long_name_with_more_than_27_chars");
    CHECK(std::memcmp(key.text, "a_very_long_name_with_more_", 27) == 0 && key.text[27] == 0);
    CHECK(cod3::generateHashValue("A") == ((97 * 119) & 255));
    CHECK(cod3::FS_HashFileName("a.b", 1024) == cod3::FS_HashFileName("A.c", 1024));  // up to the '.'
    CHECK(cod3::FS_HashFileName("a\\b", 1024) == cod3::FS_HashFileName("a/b", 1024));
}

// The 2D color and the NGL quad's defaults.
void Renderer2D() {
    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    cod3::RE_SetColor(white);
    CHECK(cod3::tr_color2D == 0xFFFFFFFFu);
    const float red_half[4] = {1.0f, 0.0f, 0.0f, 0.5f};
    cod3::RE_SetColor(red_half);
    CHECK(cod3::tr_color2D == 0x7FFF0000u);  // ARGB, 127.5 truncated
    cod3::RE_SetColor(nullptr);
    CHECK(cod3::tr_color2D == 0xFFFFFFFFu);
    cod3::nglQuad q;
    cod3::nglInitQuad(&q);
    CHECK(q.verts[3].u == 1.0f && q.verts[3].v == 1.0f && q.verts[0].u == 0.0f);
    CHECK((q.flags & cod3::NGL_QUAD_SORTED) != 0);
    cod3::nglSetQuadRect(&q, 10.0f, 20.0f, 110.0f, 70.0f);
    CHECK(q.verts[1].x == 110.0f && q.verts[1].y == 20.0f && q.verts[2].x == 10.0f && q.verts[2].y == 70.0f);
    // The PowerPC's own NaN for infinity - infinity, and NaN operands quieted.
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(std::bit_cast<std::uint32_t>(cod3::ppc::fadds(-inf, inf)) == 0x7FC00000u);
    CHECK(std::bit_cast<std::uint32_t>(cod3::ppc::fadds(cod3::ppc::f32_bits(0xFF800001u), 1.0f)) == 0xFFC00001u);
    CHECK(cod3::ppc::fadds(1.5f, 2.25f) == 3.75f);
    // The far plane: r_zfar (8192 by default), never nearer than the floor.
    cod3::RE_SetZFarMin(-5.0f);
    CHECK(cod3::tr_zFarMin == 0.0f);
    CHECK(cod3::RE_GetZFar() == 8192.0f);
    cod3::RE_SetZFarMin(10000.0f);
    CHECK(cod3::RE_GetZFar() == 10000.0f);
    cod3::RE_SetZFarMin(0.0f);
}

}  // namespace

int main() {
    ConversionsSaturateLikeThePowerPC();
    FusedMultiplyAddRoundsOnce();
    ThePlatformsDoubleFmaIsExact();
    AnglesInSixteenBitSteps();
    Vectors();
    RuntimeSinCos();
    FastYawIsCloseToAtan2();
    ScriptRandom();
    Strings();
    InfoStrings();
    Hashes();
    Renderer2D();
    if (failures != 0) {
        std::printf("%d checks failed\n", failures);
        return 1;
    }
    std::printf("all unit checks passed\n");
    return 0;
}
