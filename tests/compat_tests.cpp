// Known-answer tests for src/compat.cpp, the import of Codex Switcher backups. A console program
// of its own, from a "x64 Native Tools" prompt in the project folder:
//   cl /nologo /std:c++17 /EHsc /W3 /utf-8 tests\compat_tests.cpp src\compat.cpp bcrypt.lib /Fe:<dir>\compat_tests.exe /Fo:<dir>\
//
// Where the vectors come from:
//  - ChaCha20 block, Poly1305, AEAD: RFC 8439 §2.3.2, §2.5.2, A.3 (#5-#10), §2.8.2.
//  - HChaCha20 and XChaCha20-Poly1305: draft-irtf-cfrg-xchacha-03 §2.2.1 and A.3.1.
//  - PBKDF2-HMAC-SHA256: Python's hashlib.pbkdf2_hmac.
//  - zlib streams, the CSWF file and the css1 texts: made with Python's zlib, hashlib and
//    cryptography (ChaCha20Poly1305 under an HChaCha20 subkey) the way Codex Switcher makes them;
//    the RFC and draft values above were checked against the same libraries.
#include "../src/compat.h"
#include <cstdio>
#include <string>

static int checks = 0, failures = 0;

static void Check(bool ok, const char* name) {
    checks++;
    if (!ok) {
        failures++;
        std::printf("FAILED: %s\n", name);
    }
}

static int Nibble(char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }

static std::string Hex(const char* hex) {
    std::string out;
    for (; hex[0] && hex[1]; hex += 2) out.push_back((char)(Nibble(hex[0]) << 4 | Nibble(hex[1])));
    return out;
}

static std::string Range(int first, int count) {
    std::string s;
    for (int i = 0; i < count; i++) s.push_back((char)(first + i));
    return s;
}

// The same generators as in the Python script that compressed them.
static std::string Words() {
    static const char* words[] = { "account", "token", "switch", "codex", "limit", "refresh", "window", "plan",
                                   "usage", "reset", "quota", "email", "workspace", "profile", "backup", "import" };
    std::string s;
    uint32_t x = 12345;
    for (int i = 0; i < 2500; i++) {
        x = x * 1103515245u + 12345u;
        s += words[(x >> 16) & 15];
        if ((x >> 8) % 13 == 0) s += std::to_string((x >> 4) % 1000);
        s += (x >> 24) % 9 == 0 ? ".\n" : " ";
    }
    return s;
}

static std::string Binary() {
    std::string s;
    for (uint32_t i = 0; i < 6000; i++) s.push_back((char)((i * i + i / 7) & 0xff));
    return s;
}

static const char kLadies[] = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";

static void ChaCha() {
    using namespace Compat::Detail;
    // RFC 8439 §2.3.2
    Check(ChaCha20Block(Range(0, 32), 1, Hex("000000090000004a00000000")) == Hex("10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4ed2826446079faa0914c2d705d98b02a2b512"
        "9cd1de164eb9cbd083e8a2503c4e"), "ChaCha20 block, RFC 8439 2.3.2");
    Check(ChaCha20Block(Range(0, 31), 1, Hex("000000090000004a00000000")).empty(), "ChaCha20 block refuses a short key");
    // draft-irtf-cfrg-xchacha-03 §2.2.1
    Check(HChaCha20(Range(0, 32), Hex("000000090000004a0000000031415927")) == Hex("82413b4227b27bfed30e42508a877d73a0f9e4d58a74a853c12ec41326d3ecdc"), "HChaCha20, xchacha draft 2.2.1");
}

static void Poly() {
    using namespace Compat::Detail;
    // RFC 8439 §2.5.2
    Check(Poly1305(Hex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b"), "Cryptographic Forum Research Group") ==
              Hex("a8061dc1305136c6c22b8baf0c0127a9"), "Poly1305, RFC 8439 2.5.2");
    // RFC 8439 A.3 #5-#10: the edge cases of the final reduction and carries.
    static const char* edge[][3] = {
        { "0200000000000000000000000000000000000000000000000000000000000000", "ffffffffffffffffffffffffffffffff", "03000000000000000000000000000000" },
        { "02000000000000000000000000000000ffffffffffffffffffffffffffffffff", "02000000000000000000000000000000", "03000000000000000000000000000000" },
        { "0100000000000000000000000000000000000000000000000000000000000000", "fffffffffffffffffffffffffffffffff0ffffffffffffffffffffffffffffff11000000000000000000000000000000", "05000000000000000000000000000000" },
        { "0100000000000000000000000000000000000000000000000000000000000000", "fffffffffffffffffffffffffffffffffbfefefefefefefefefefefefefefefe01010101010101010101010101010101", "00000000000000000000000000000000" },
        { "0200000000000000000000000000000000000000000000000000000000000000", "fdffffffffffffffffffffffffffffff", "faffffffffffffffffffffffffffffff" },
        { "0100000000000000040000000000000000000000000000000000000000000000", "e33594d7505e43b900000000000000003394d7505e4379cd01000000000000000000000000000000000000000000000001000000000000000000000000000000", "14000000000000005500000000000000" },
    };
    int good = 0;
    for (auto& e : edge) good += Poly1305(Hex(e[0]), Hex(e[1])) == Hex(e[2]);
    Check(good == (int)(sizeof edge / sizeof edge[0]), "Poly1305 edge cases, RFC 8439 A.3 #5-#10");
    Check(Poly1305(Hex("00"), "x").empty(), "Poly1305 refuses a short key");
}

static void Aead() {
    using namespace Compat;
    const std::string key = Range(0x80, 32), aad = Hex("50515253c0c1c2c3c4c5c6c7");
    std::string plain;

    // RFC 8439 §2.8.2
    const std::string sealed = Hex("d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b1a71"
        "de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def0"
        "8e4b7a9de576d26586cec64b61161ae10b594f09e26a7e902ecbd0600691"), nonce = Hex("070000004041424344454647");
    Check(Detail::ChaChaPolyOpen(key, nonce, aad, sealed, plain) && plain == kLadies, "ChaCha20-Poly1305, RFC 8439 2.8.2");
    std::string bad = sealed;
    bad.back() ^= 1;
    plain = "stale";
    Check(!Detail::ChaChaPolyOpen(key, nonce, aad, bad, plain) && plain.empty(), "ChaCha20-Poly1305 refuses a flipped tag bit");
    Check(!Detail::ChaChaPolyOpen(key, nonce, "", sealed, plain), "ChaCha20-Poly1305 refuses other associated data");

    // draft-irtf-cfrg-xchacha-03 A.3.1
    const std::string xnonce = Hex("404142434445464748494a4b4c4d4e4f5051525354555657"), xsealed = Hex("bd6d179d3e83d43b9576579493c0e939572a1700252bfaccbed2902c21396cbb731c7f1b0b4aa6440bf3a82f4eda7e39ae64"
        "c6708c54c216cb96b72e1213b4522f8c9ba40db5d945b11b69b982c1bb9e3f3fac2bc369488f76b2383565d3fff921f9664c"
        "97637da9768812f615c68b13b52ec0875924c1c7987947deafd8780acf49");
    Check(Detail::XChaChaPolyOpen(key, xnonce, aad, xsealed, plain) && plain == kLadies, "XChaCha20-Poly1305, xchacha draft A.3.1");
    Check(!XChaChaPolyOpen(key, xnonce, xsealed, plain), "XChaChaPolyOpen (no AAD) refuses the A.3.1 message, which has AAD");

    // The same inputs without associated data, through the public function.
    const std::string open = Hex("bd6d179d3e83d43b9576579493c0e939572a1700252bfaccbed2902c21396cbb731c7f1b0b4aa6440bf3a82f4eda7e39ae64"
        "c6708c54c216cb96b72e1213b4522f8c9ba40db5d945b11b69b982c1bb9e3f3fac2bc369488f76b2383565d3fff921f9664c"
        "97637da9768812f615c68b13b52ef7e62efbf45089db18f9c8a3f0e41e5f");
    Check(XChaChaPolyOpen(key, xnonce, open, plain) && plain == kLadies, "XChaChaPolyOpen without AAD");
    for (size_t i : { (size_t)0, open.size() / 2, open.size() - 16, open.size() - 1 }) {
        bad = open;
        bad[i] ^= 0x80;
        plain = "stale";
        Check(!XChaChaPolyOpen(key, xnonce, bad, plain) && plain.empty(), "XChaChaPolyOpen refuses a flipped bit");
    }
    Check(!XChaChaPolyOpen(key.substr(1), xnonce, open, plain), "XChaChaPolyOpen refuses a 31-byte key");
    Check(!XChaChaPolyOpen(key, xnonce.substr(0, 12), open, plain), "XChaChaPolyOpen refuses a 12-byte nonce");
    Check(!XChaChaPolyOpen(key, xnonce, open.substr(0, 15), plain), "XChaChaPolyOpen refuses less than a tag");
    Check(!XChaChaPolyOpen(key, xnonce, open.substr(0, open.size() - 1), plain), "XChaChaPolyOpen refuses a truncated message");
}

static void Pbkdf2() {
    // hashlib.pbkdf2_hmac('sha256', password, salt, iterations, len(expected))
    static const struct { const char *password, *salt; unsigned iterations; const char* expected; } vectors[] = {
        { "password", "salt", 1, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b" },
        { "passwd", "salt", 1, "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783" },
        { "Password", "NaCl", 80000, "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d" },
        { "password", "salt", 4096, "c5e478d59288c841aa530db6845c4c8d962893a0" },
    };
    for (auto& v : vectors) {
        std::string want = Hex(v.expected);
        Check(Compat::Pbkdf2Sha256(v.password, v.salt, v.iterations, want.size()) == want, "PBKDF2-HMAC-SHA256 vector");
    }
    // The key of the CSWF file below: the app's passphrase, its salt, 210000 rounds.
    Check(Compat::Pbkdf2Sha256("gT7kQ9mV2xN4pL8sR1dH6zW3cB5yF0uJ_aE7nK2tP9vM4rX1", Hex("c46f0a10c68841d4b6be35387eb08fc8"), 210000, 32) == Hex("842bac354dfca84b3a7d66030686eb5188250c37647ca86991e4f0e9c1151915"),
          "PBKDF2 key of the Codex Switcher file");
    Check(Compat::Pbkdf2Sha256("p", "s", 0, 32).empty(), "PBKDF2 refuses zero iterations");
}

static void Inflate() {
    using Compat::ZlibInflate;
    std::string out;
    const std::string stored = Hex("7801013900c6ff53746f72656420626c6f636b3a207a6c6962206c6576656c2030206b656570732074686573652062797465"
        "732061732074686579206172652e505c143c"), fixed = Hex("78daf348cdc9c9d751c840a214151c158a33f28b4a148a4b8a32f3d215d2534b8a15d2322b5253143c4ad3d27213f31492f3"
        "53528bf50090a31582"), dynamic = Hex("78da7d5c5976e4380efcf729f2047e12256a398ec7ed9ef6abc59e2ad7731f7f940248460421ffd8954e7103814060517dbe"
        "fdfaf6fbfde9f9e5f6fdf5c7ebc7edd7cbdfbf5e7eff73fbdf9fb78fa7dbeb8ff7b75f1fb7f75f6f7fbf7e7fa9bfcf2fd398"
        "fda932e6b3cef5f2e3e9f5fbed3f4fcfdffebc97499edffe7af9f7f6f4fcfcf6e7e7c7e3c331e2e5038694b9fd8132ea7cec"
        "f1c13f952fcbc3b6e74f3943f9f6f7e7ebc7f33fbe99cfd79f7fbd7ddefefc7efaef8befe5dcfdbecde5415fc47ff9eedfbf"
        "3ffdf451be978fb76f2f3f1f1ffc7b3b473d962d569e37b9f4f2b141f6f39cae2ceabbf45fbe2f96639da5c8b04c5f8e6d5b"
        "3051d846ca3591c4e771f4874c1abc98ef80eecea629cb5c493d120f5fdfe383cf7ecab6cdd3cd78ffdab75546748b1795b0"
        "856c8be770174f5dcc7ff14d3d3ed8d1fc4b5bd797f45f2ede794f22611b796eb23c9327df01fec43b30a91435c13bf21db8"
        "d86c6efb30cd3b5ec0e3c339e0f1418dc1e60105dcf6a9481ee4b24c9b9cb009d3a6388fe456d3ecf3fe4798e6e6bbb039ee"
        "c2362db471f6d37796b6b15e934c4867f7bdfaaef8586625f6d3ae9a24e5ab4f69f3a5f9f26c311f6187b0a91c40101bee8f"
        "e643460a50f628211bdb9fcde80bdbd3b8885f1e982f9aa5adcd187c6e5ac0cc9e63c062d1d70596bc9255283c97610c2fa8"
        "adfda5c1bf4e99553b079db1b1e7bfd769a80728aa7f3e69b786c003921120eeb41466286740a83f9628fe0a6c531d0b40fa"
        "36eee2ef9aae9370708f2659b85a017d524fd6e5dee9e1860b38e0028084fbb016d03abfdbc7858dbcce9ee701d5b00106a9"
        "bb6e89dc917df0a3b8c193eea349b505ec00e51362a12fedb64588acda7cda61ea74979d8d090205061026f3fbc7f3e13c13"
        "aeb0132993db27c64a00ee228a8281e511c4174203175659acfc862b262d0c20934ca9fc12a84331d04ddb8edc264ecfe8be"
        "4fee3ed45c3ac895fb38bfcd4b763d6633eef18fa906ba3cfb306fd579bb3fb9efbdbabdfb8700948b8f3069f84d55bdb3bf"
        "9a40587e655eba383b08d984edd33f44a49944cf4c8eb0c7ff76ceb71e649aa8120f38cfea56ec8b31f2225b26d0b7dd82a9"
        "476e87e1499978e55c8ef8c833aaeedd61744facc0263675199d452b40839e9e4bed87536f5acbd4c2cfe743e86fcd8a957d"
        "bb208901747c52ffce77cc9fd0e2fd9bb1a2b439c22d0bc5efc1a2712891fc3a67209b8f0f11b5e7817ed3e720f3b8e56779"
        "22d67de4f9be278a9e48ebd16e1b16daf31c38169d738520cb287efcb8da699f7d6d22fd95c8934369d232b5a0f012d0b4c2"
        "90c848a0a5231f1a66a1eff78909e5e03ab6740307ddb803bb239f446e40c82f868826e1e58857e91a0c7427629c14d428be"
        "17ccaa52ebb646b6cd5605a8b2cc4b048d799ec82d5779b3fe4b942d4488d91df1391f81c7653e0b4652c50ba222aa8dd486"
        "a20bbc3a3a0eb3bc265b66d18c653dad2c1ae407b307721426913d69f0302f4b7c690c0baae21ada441128d92ab926d0a2e2"
        "0c8aa15ee565449538bae041104854cdb11b45a827a2d6d60db84f47522eb2548c1f9c3a505742a98e1abf083b3ba74893c6"
        "244c7a7826fb6e9bc4bdc43128c1e87c44017d6e04c375f4d482ea627ccb9e294a23716a4eeb5c220f3be2b8af58afaf0808"
        "1909530ed517f6e73d015685b6591852ecd77808069d3407eb3e82a4a173934be911941920194a9bc361f6dce6ca341b7249"
        "d56117738a9496920301e4992c9675b0032f6b6e6642d4d539026213b148a233d5f94efb746959e2ca3874c8c324d7caa117"
        "2666e5fa892f4076a18c5d96c187e024bd53c847d4e7ac86519cf188a32b49339db073cc35afcbad8fd3019f6536bb21b567"
        "0d053ba681076293e1cc474d67cad034e54ea15d90a8caae776a86e06f0b6430fb26ee23d1e4bacc37649ec231487ff1c604"
        "683a5bc27d8bc691072bdca46376b2791b340d9326209044ade3d8c74560329807d254b5b23022cc8425ce0a109bd2b695bc"
        "fbb1dcbde6c2fe9e3c9e38787f721f6f71a01f5e057f858cc9af51cb0fc278103c98ea91cf9ed7b9126b227df7d42bb327f2"
        "261d533ce718f7cc6a1da72624cee165240d1571cae2bb3b3026f86fb6cfb1bc5be8b9933149f2384cb437b871875cd118d3"
        "e11079a4945b5cd483e1454c5b0fe62e016308cb411f3f9683ca08a651c6c60621e3881285ac571daf2e0829964aea5c3528"
        "e7ea30813a9e403f84e84d293c4a01a1db51b844e2c4243bd27cae6d5511b89c455fe67525fb8792085c3ab230a638902989"
        "6aad412252728544eda22cf61588ad23e6dfd027f7997dff352f2ba671a2988e5914d245cafadeeb49b231d2358add481184"
        "f8ea29b946272aebd74b9353b6026dac8fe37a43c708367664a01bcb4ad2435cdae6c4ae93a198928d98bb93280c10ce4f99"
        "d7cc3558e52d35bcee8031c8f91bba6f4bf1721e40524e170ca5f322c0c018b0a45c243244f9268892a2cc1d82f79ef32de4"
        "43cd549765e76c30b90ed6bf52a0dc27dad1717574480a4b38718e006bf9d6750f6acb52c7273ca5009e6cbf8608bd9ab2ad"
        "e471ebae394c7628e5a0601bed663a8ed1643aa6127c12dfc44a2857972024def2da45cc6ae9ea5c7b43e589d9952ff38a05"
        "261635a74228e50e3e619f46b2c230f395f6a985cde4a308ababc363f336e31de730e9c1510491d0f3e7ba8d1ca9db8a691a"
        "2eca88e86b84e45e79512418b8147a8d75def1b668dbc518aab282cbeda0849db59f66d660944a1f691c50f1e26abb461717"
        "89c1d67f72957e737d47a560f682096e723ce4f4534a54d20d729738f85e55e1ec2485e5a6aa626b9c4f74000f12efea94a1"
        "14574611b5132fd0456c24878e77b43fac7baaa9d4a1683117b528c94336e0abe73d154547a524c7c8a88a1717dff5386f0a"
        "3768c61705b17b00dd8a15a23d64b8289f290dc244d1e6a8f425614a185991cfa851886429a2708a75006584ec8e4aeca74f"
        "9d260616d797bead4a93d8ecfece4ddc4b1194102fe0cc27de0e9189a3aac243975e409ddbd6ca6c0d94a2889cd181927edc"
        "e186b4a8c7460aa5a9d9ed5c7c5ba62eed311f407a9dfae730b7f39b88d15d26c6c606f576f4dd3965e15cecf7ac536d497e"
        "7f7b9efa853a40c05538af1b44007d134b6f6bcb9e1a26d5f42b1538b5b7c8a3d0394ee547553372410881160cce63ed2d55"
        "1963fd9f2319f614bc63f203dc48411742c4aaa973d43ecb7539732407b809a8a32190388a1d83972879d474f06d898dca19"
        "2929dc2838454e6c831ac393fb43225df8599f5e1730ecb3cfb50f93c9b50c389f998625aea25cb1ce7399e91ef2f512ddd3"
        "5a8e298dc1d8ec438417b08a2411d67939bf486a43f1fc3e66e1c8d4d525b90c6d24b2a79693fb73ef8206c8414e3dee4c61"
        "b2920f99c35066016c9d1deaa1b792355b8424b4619a83202ae82aa614a4b670f7b855762efd011e13f6bd48f33e849db192"
        "1b18d78d4034ec3a8236872023b16feb4534926adb5edfb6cc70ad15704669c22ff41dd2f519712176dff6eb9e810a3ca7c8"
        "a957376cd4eafe1ab6326a675e7b24e5a5f0b491d90f08f1ebf22fab00bb5f86912e2d832d638a746ccbd437c1fc8e76c127"
        "fdaa60d9d8fb36cd347f5ce8ec0c8ffa0ec4e98aa424a0e8bc2a963534870fc987ee54e441e876439b23c5e40c2cb41f1f1e"
        "10d6548242a95ef6f6e45ac7652d68126653bba43e12e5b6de98762dbd49b25877063987867cf4c2086478b9001364833046"
        "98f366b3cf69119a49ea49b6ee7a92a7356c400edbad38dfa8e7449a8921958fda0e371d2582447a343ec8106edb40c13367"
        "c9396f5ff65baebea27adea336fe2fdb7ca1ada1e3dd4c0ffd8a118fc7e386b03c11954e25f6f3035ce4b1e4bc671662dfb5"
        "7f3eb27d298db52991ee49c4cb5e4faa4c5f25932ee325cd18539387d34642059683c714a398601a36752d588e4290c6fc44"
        "982443320d1999f61601c6155a4c541dead07a4c334b0eab0fdb30c99b0e1a975e67ab7d1fdb20dd8f7d9b1af7a4392b5a93"
        "92466d3fa7089f58595a2f7b823034dc73ea9a32fad746c817100a920741ff7d303ae479691911c0e5858cf0b598f3507770"
        "c077ded603b748c49058c3b67720f2b88d69ca91a4b5db8c1a104be05b6dd3fabbb7a57f1d945a4a3868a22c42b3701343da"
        "56040fe9b2edb2136ca01a5f50af03bd4f6032d806af666de320d6061da5d396c88748bf23231ec89c93745e2c1957ec9344"
        "0c690db6c1eb7c5de9598c0c7d7435109463c9496eb9eb1a443249c5c6986fc5c964e12cd1dba35453e6d7cf6a2340b88fd2"
        "d7986b262a0f63d846f5554d56322d92fc0c5bc8dcdc8879c0d5725b57df7dcc994824078ac35d2348c0baf5055d6c5fba20"
        "756c71ec84054e0f23960882ee5564c98987467ed1578bd55ff6de44aad23724b0543c7343e7abb168d7bb77f5fe76d0f9c2"
        "39707acd7cd0d79de16ae4fdb37b7fe1b05ea0617d5d92f2c59c75e40b433917bb518f061ae4c694a7dd26dfd2da823081c8"
        "7c6fc2619fde07171d1c04f93b0a31e77d8bdf392dff4ac344f16fdb57187a05edebd79caf4888adb3cb5bea7be55745818b"
        "82b45425fb3611d67e8c0da357f2f41d997d495c2611dc68c0095471dc36c49f2d4d7c600ad7d4e9208f45125aae7420b71b"
        "230b42121a6f106263f4426cecfedf1a0471e5382c1c60d335f43953aef469a50ca0b878946556d283b8a0afb9344b41e063"
        "bcd9f72455b472679afffca260428c1ad7e2483c7a5d5e629969992f5e148c8271cce09133763dc5c08f4bfbb2ec5916b928"
        "00059d640e0748f4f0d539421b7c5bd2adc8a28579166685cf03f1d47c1327312f8d9a9c1477b5d8cb828b306b6dacbfaff5"
        "7fd5f7bd19");
    Check(ZlibInflate(stored, out) && out == "Stored block: zlib level 0 keeps these bytes as they are.", "inflate a stored block (level 0)");
    Check(ZlibInflate(fixed, out) && out == "Hello, hello, hello! A short string gets fixed Huffman codes.", "inflate a fixed Huffman block");
    Check(ZlibInflate(dynamic, out) && out == Words(), "inflate a dynamic Huffman block (text)");
    Check(ZlibInflate(Hex("78daedd5e3421e0a0000d0bc6ad9b66ddbb66ddbae65dbb66ddbb56cdbb656f725eecfefbcc40102068342c02461125432f5"
        "4aa81c3d042116b34eec3e45117629db42910d1f01130d9f41d5ab78114dbde04fbd952a83b098624803b25e93e8a029c048"
        "8089848a844dc42ca4eb92dab403cd6499b5f95d2dff942d7a9723f3afde0c7725410e7626511defb4d15f8ee0713ccf4399"
        "01e17ea9233fc2c5485188958a100b0d1909190d9b88865d44d9c43dbab867ed198555c3af64e69d4c35b4f3964c3f73154d"
        "236b8fdca10b52a3e24bb11c58a713d36b5ba802c9ef526b8aaf89aa183f1b3b1bbf98a645e0afce75005a8dd8be1f6eafd1"
        "af5a75903a3d84210ffab3220382fd624b86f7e1444326f08d8648336122d093a9c6ec30176315711e165a0a5212520a3a56"
        "3e493553b7c8bcd6d98b5f6412d6f16d7bbf392c32a681391cab2f28ad6a1e380367b0adbae08c3b51eca7a9a36f55ba49e3"
        "ffd96c2b8cf2b632d6d2d632b5f18d29edda012551f4693cc633ccbb60f65ba20c3a122d470a7eb43a333db67a0bc1ac163b"
        "8ba05f0de778a8b2a0beed8cd46684ba9a69c98bf173b1b371700b882b685b784464378cef7f61716af9164e3ce0497b55ed"
        "204b05f77d71fbf481c9a4ecd1fafc2570fb4bf667873717c47c86bd10deefce784d7e5cb85f6040644261dde4e10f5209d7"
        "bc154491d06128f5820ff95210cb69b60aec788808f878ac2ade796b881ad5cf4a5da4b918c5efbbd5feaaf4704f1b430d45"
        "1929492959254d233befc8acdaa18d27781a699bb8e62d7006dde89e3b32ddd47938d9d80574bdb207fed83de6f05d8ea43b"
        "850614cf3dc966a20410d70bc30db545a545b54da32b77b024923699235f8c1695e774bee358ce9314b18f3a33fc8d34358c"
        "cd7c7386cff1744b1ec4b3816c2f4d2e5c90f385deaacc283f672b137d1d6d1d7d131bd78098ec9afe956b08423e5dbfc2b1"
        "1b7421fbdc39500efbf243028382234a870e30a58207d1cc7ba95250a321b2a8079d31962294b02f976b8b6393638b1bd7ef"
        "08b412b649fd9738aa3133c82a85d77ce9b7724d98010fc6ab4a4baaeb464f21b8cd4a6f39e28fa5ba686b197a94afe3b99e"
        "ea9cf8505f37473b5b3b2777bfb0c4bcdafec5b31f0c66058784e675204a95c0860354a93f9d2fcc4e4d2fdc81d3e8264d60"
        "ead560baeda82ecb1cb9e076eb622d6499f0a13f5eaf6e6fde4061085914ed925b8e90053c1b9e583d46508d7abf3badf116"
        "23457ebabd797e86a35509aebb630cd8604f06727df584c8e15e0f667beb8bd2e3c2830202fe0447c46714370e2c1c7f2052"
        "891907168d5e22f198270f3d91eb24ff85140d1e81944bd9a1741b41b3ea43b39f240f3d93ae470fb8d418e7ac264c478c86"
        "8e808e41ca20aae59ed4be09c26a96bf8aa29975c11971c09df66538c951439c8991435acd3567fa99c97796c07f5bac8f77"
        "54e124807a3a981f6ca92cc84c4d4a4c4acd2aac6e1b5e3c7a8521e1d5704d6e59fb2695f32c5d86e0b22fddc5524f5dc336"
        "28bbe30e5fa3f499a7f8b3c595f1a13b4899f86dbd21ddcb5c4992879385934752cdda2fbb650b944a3b6af89dc3771046b7"
        "1a48bf132ffc566741bc976f48725eef2982b4df18a2c5087d3254186426cd84f3ebe97069acbbb9a6bca4b8a4bcb6a57762"
        "e5f8051a9f4dde2aac74fc1281dd20aafd0c53e64ffb3d8d55c505ad5307885cd60577ec2157f2ad4c158cfd124f0982ef95"
        "ee9c580f5b035d0d5d237baff882fe4d007239ff9a3342bde25386c079b2d07da12af48017db63a333dbe760b43ac9e3709a"
        "8d08eedb72738a879e281d26c4f3c9c6ec685f675b6b7b57ffd8dce6d91b140ea384a14f66c7c63f2249a7dca90f6abda449"
        "307e9fae2ffee06954839a2fd9fc17d95260bd1ebcc053b936a2b86f87438d598921817ec161c939cd2327e044d24ec58b50"
        "e24163882aa5af3215d066739ca51809f0e11009b8e55c4b36f0954a7f357ae8d3d1f2280775016a0cc8f7abfdb5f9e9c989"
        "c9bf0beb07379fd0d874c25a2e71d55357f02c3a21f5db30bc0ea5db688ab17fe194d276c89dfae00c9a20f5db911de61893"
        "df0d26580a11039fadf6b55694179457b40f6c5e83918bd9a74dfea5b32eb962f49ac27598a18e7ad19fe469a1afa46b139c"
        "d67b4b609c77275808e27cac77e6045a2af15163fdfe7ebc38dadddadcda3dba7a0682c5a11550b30d2de8ddfe2612b74b1b"
        "bcc553086abfa530cedf2334a9b8e30c5e20741ac1729c240b3a102efdedb425de4c9cfccbe7c9f6ccf4c8f4cceed90f2a95"
        "b4556ad70dbe52fc249c7ace9d60ca83623d5af0adc186eabcd296e97d30569bea73a6c84b851ee672ba2ed9c75481970617"
        "01e2dfcbfdcdf5cdc3ebf72f247c3a01650bfff4a6b93b4416cd808a251046a39449602eb7a60766b72e1099b423e6e0559a"
        "e06d8e9427d576bc901bcd51d652ac3808ff0ff7178fcf805ff1d8e51c133af7e184dd5aef99bc07114c3b515c97b9cad0c3"
        "fe3c9fddfe2230cbf9b63cb07bad31668278ff7606cb62dd8de4f9188831e02141818000ff01fe03fc07f80ff01fe03fc07f"
        "80ff00ff01fe03fc07f80ff01fe03fc07f80fffee7fffe03886b98fb"), out) && out == Binary(), "inflate a dynamic Huffman block (all byte values)");
    Check(ZlibInflate(Hex("78daf248cdc9c9d751c840a214151c158a33f28b4a148a4b8a32f3d215d2534b8a15d2322b5253143c4ad3d27213f31492f3"
        "53528bf500000000ffff7d5c5b76dd380efcf72aee0a7c244ad463399e8c7bda27493b933827b3fc910490ac2a42feb1737d"
        "c51708140a0fe5cffbcfafbf7ebc7c797d7c7bfbfef6f1f8f9fad7cfd75f7f3ffefbfbfde3e5f1f6fdc739c98f9fef7fbd7d"
        "7badbfaf2fd398fda932a6cdf5fafde5eddbe35f2f5fbefefe512639d7fbdfe3e5cb97f7dfff7c3c3f1d235e3f604899db1f"
        "28a3aec79e9ffc53f9b23c6c7bd633946f7ffd79fbf8f2b76fe6cfdb3fff7efff3f8fdebe53fafbe976bf7fb3697077d11ff"
        "e5bbfff1ed90958df2bd7cbc7f7dfde7f9c9bfb773d463d962e579934b2f1f1b643fafe9caa2be4bffe5fb6239d6598a0ccb"
        "f4e5d8b60513856da45c13497c1e477fc8a4c18bf90ee8ee6c9ab2cc9dd423f1f0f53d3ff9ec976cdb3cdd8ce7d7bead32a2"
        "5bbca8842d645bbc86bb78ea62fe8b6feaf9c98ee65fdababea4ff72f1ce7b1209dbc86b93e5993cf90ef027de8149a5a809"
        "de91efc0c56673db8769def1029e9fae01cf4f6a0c360f28e0b64f45f2209765dae4844d9836c57524b79a669fe71f619a87"
        "efc2e638856d5a68e3eca7ef2c6d63bd269990ceee7bf55df1b1cc4aeca75d3549ca579fd2e64bf3e5d9623ec20e61533980"
        "20369c8fe643460a50f628211bdb9fcde80bdbd3b8885f1e982f9aa5adcd187c6d5ac0cc9e63c062d1d70596bc9255283c97"
        "610c2fa8adfda5c1bf2e99553b079db1b1d7bfd769a80728aa7f3d69b786c003921120eeb41466286740a83f9628fe0a6c53"
        "1d0b40fa36eee2ef9aae9370708f2659b85a017d524fd6e5dee9e1860b38e0028084fbb016d0babedbc7858dbcce9ee701d5"
        "b00106a9bb6e89dc917df0a3b8c193eea349b505ec00e51362a12fedb64588acda7cd961ea74979d8d090205061026f3fbc7"
        "ebe13c13aeb0132993db27c64a00ee228a8281e511c4174203175659acfc862b262d0c20934ca9fc12a84331d04ddb8edc26"
        "2ecfe8be4fee3ed45c3ac89dfbb8becd4b763d6633eef18fa906ba3cfb306fd579bb3f39f75edddef92100e5e2234c1a7e53"
        "55efecaf2610965f99972ece0e423661fbf40f116926d1339323ecf1bf5df3ad079926aac403aeb3ba15fb628cbcc89609f4"
        "6db760ea91db617852265e3997233ef28caa7b278cee8915d8c4a62ea3b3680568d0d36ba9fd70ea4d6b995af8f97c08fdad"
        "59b1b26f172431808e4feadff98ef9135abc7f3356943647b865a1f83d58340e25925fe70c64f3f929a2f63cd06ffa1a641e"
        "b7fc2c4fc4ba8f3cdff744d113693dda6dc3427b9e03c7a273ae106419c58f1f573bedb3af4da4bf127972284d5aa616145e"
        "029a5618121909b474e443c32cf4fd3e31a11c5cc7961ee0a01b776077e493c80d08f9c510d124bc1cf12a5d8381ee448c93"
        "821ac5f78259556addd6c8b6d9aa0055967989a031cf13b9e52a6fd67f89b2850831bb233ee723f0b8cc67c148aa78415444"
        "b591da5074815747c76196d764cb2c9ab1aca7954583fc60f6408ec224b2270d1ee665892f8d6141555c439b2802255b25d7"
        "045a549c4131d4bbbc8ca81247173c080289aa3976a308f544d4daba01f7e948ca4d968af1835307ea4a28d551e3176167d7"
        "1469d29884490fcf64df6d93b897380625189d8f28a0cf8d60b88e9e5a505d8c6fd9334569244ecd695d4be461471cf715eb"
        "f51501212361caa1fac2febc27c0aad0360b438afd1a0fc1a093e660dd479034746e72293d8232032443697338cc5edb5c99"
        "66432ea93aec624e91d2527220803c93c5b20e76e065cdcd4c88ba3a47406c22164974a63adf699f6e2d4b5c19870e7998e4"
        "5a39f4c2c4ac5c3ff105c82e94b1cb32f8109ca4770af988fa9cd5308a331e71742569a60b768eb9e67579f4713ae0b3cc66"
        "37a4f6aca160c734f0406c329cf9a8e94c199aa6dc29b40b1255d9f54ecd10fc6d810c66dfc47d249a5c97f981cc533806e9"
        "2fde98004d674bb86fd138f260859b74cc4e366f83a661d2040492a8751cfbb8084c06f3409aaa5616468499b0c459016253"
        "dab692773f963b6b2eecefc9e38983f727f7f11107fae155f057c898fc1ab5fc208c07c183a91ef9ec799d2bb126d277a65e"
        "993d9137e998e235c7b86756eb383521710e2f2369a8885316dfdd8131c17fb37d8ee5dd42af9d8c4992c761a2bdc18d3be4"
        "8ac6980e87c823a5dce2a21e0c6f62da7a3077091843580efaf8b11c5446308d323636081947942864bdea78754148b15452"
        "e7aa4139578709d4f102fa21446f4ae1510a08dd8ec225122726d991e6736dab8ac0e52cfa32af2bd93f9444e0d2918531c5"
        "814c49546b0d1291922b246a1765b1ef406c1d31ff863eb9cfecfbaf7959318d13c574cca2902e52d6f7ac27c9c648d72876"
        "234510e2aba7e41a9da8ac5f2f4d4ed90ab4b13e8eeb0d1d23d8d891816e2c2b490f71699b13bb4e86624a3662ee4ea23040"
        "383f655e33d76095b7d4f0ba03c620e76fe8be2dc5cb790049395d3094ce8b000363c0927291c810e59b204a8a327708de7b"
        "ce8f900f35535d969db3c1e43a58ff4a81729f6847c7d5d121292ce1c43902ace55bd73da82d4b1d9ff0940278b2fd1a22f4"
        "6acab692c7adbbe630d9a19483826db49be9384693e9984af0497c132ba15c5d829078cb6b1731aba5ab73ed0d95276657be"
        "cc2b169858d49c0aa1943bf8847d1ac90ac3cc57daa71636938f22acae0e8fcddb8c779cc3a40747114442af9feb3672a46e"
        "2ba669b82923a2af11927be7459160e052e835d679c7dba26d1763a8ca0a2eb7831276d67e9a5983512a7da47140c58babed"
        "1a5ddc24065bffc95dfacdf51d9582d90b26b8c9f190d34f29514937c85de2e0b3aac2d9490acb4d55c5d6389fe8001e24de"
        "d5294329ae8c226a275ea08bd8480e1def687f58f75453a943d1622e6a5192876cc057cf7b2a8a8e4a498e9151152f2ebeeb"
        "71de146ed08c6f0a626700dd8a15a23d64b8289f290dc244d1e6a8f425614a185991cfa851886429a2708a75006584ec8e4a"
        "ec974f9d260616d797bead4a93d8ecfeae4d9ca5084a881770e6136f87c8c45155e1a14b2fa0ce6d6b65b6064a5144cee840"
        "493fee70435ad4632385d2d4ec762dbe2d5397f6980f20bd4ffd7398dbf94dc4e82e136363837a3bfaee9cb2702ef67bd6a9"
        "b624bfbf3d4ffd421d20e02a9cd70d2280be89a5b7b5654f0d936afa950a9cda5be451e81ca7f2a3aa19b92084400b06e7b1"
        "f696aa8cb1fecf910c7b0ade31f9016ea4a00b2162d5d4396a9fe5ba9c399203dc04d4d110481cc58ec14b943c6a3af8b6c4"
        "46e58c94146e149c2227b6418de1c9fd21912efcac4faf0b18f6d9e7da87c9e45a065ccf4cc3125751ee58e7b5cc74867cbd"
        "44f7b496634a633036fb10e105ac224984755ece2f92da503cbf8f5938327575492e431b89eca9e5e2fedcbba0017290538f"
        "3b5398ace443e6309459005b67877ae8ad64cd1621096d98e620880aba8a2905a92ddc3d6e959d4b7f80c7847d2fd2bc0f61"
        "67ace406c67523100dbb8ea0cd21c848ecdb7a138da4dab6d7b72d335c6b059c519af00b7d87747d465c88ddb7fd3a335081"
        "e71439f5ea868d5add5fc35646edcc6b8fa4bc149e3632fb01217e5efe651560f7cb30d2a565b0654c918e6d99fa2698dfd1"
        "2ef8a49f152c1b7bdfa699e68f0b9d9de151df81385d919404149d57c5b286e6f021f9d09d8a3c08dd6e6873a4989c8185f6"
        "e3c303c29a4a5028d5cbde9e5cebb8ac054dc26c6a97d447a2dcd61bd3aea5374916ebce20e7d0908f5e18810c2f1760826c"
        "10c60873de6cf6392d4233493dc9d65d4ff2b4860dc861bb15e71bf59c483331a4f251dbe1a6a34490488fc60719c26d1b28"
        "78e62c39e7edcb7ecbd55754cf7bd4c6ff699b2fb43574bc9be9a15f31e2f178dc109627a2d2a9c47e7e809b3c969cf7ca42"
        "ecbbf6cf47b62fa5b13625d23d8978d9eb4995e9b364d26dbca419636af270da48a8c072f0986214134cc3a6ae05cb5108d2"
        "989f08936448a62123d3de22c0b8428b89aa431d5a8f6966c961f5611b2679d341e3d2fb6cb5ef631ba4fbb16f53e39e3467"
        "456b52d2a8ede714e1132b4beb6d4f1086867b4e5d5346ffda08f9024241f220e8bf0f46873c2f2d2302b8bc9011be16731d"
        "ea04077ce76d3d708b440c89356c7b07228fdb98a61c495abbcda801b104bed536adbf7b5bfad741a9a5848326ca22340b37"
        "31a46d45f0902edb2e3bc106aaf105f53ad0fb0426836df06ad6360e626dd0513a6d897c88f43b32e281cc3949e7c59271c5"
        "3e49c490d6601bbcced7959ec5c8d04757034139969ce496bbae412493546c8cf9569c4c16ce12bd3d4a35657efdac360284"
        "fb287d8db966a2f230866d549fd56425d322c9cfb085cccd8d98075c2db775f5ddc79c894472a038dc358204ac5b5fd0c5f6"
        "a51b52c716c74e58e0f430628920e85e45969c7868e4177db558fd6def4da42a7d43024bc5333774be1a8b76bd7b77ef6f07"
        "9d2f9c03a7d7cc077ddd19ae46de3f3bfb0b87f5060debeb92942fe6ac235f18cab9d88d7a34d02037a63ced36f996d61684"
        "0944e6b309877d7a1f5c747010e4ef28c49cf72d7ee7b4fc2b0d13c5bf6d5f61e815b4afdf73be2221b6ce2e6fa9ef95df15"
        "056e0ad25295ecdb4458fb31368c5ec9d37764f62571994470a3012750c571db107fb634f181295c53a7833c164968b9d281"
        "dc6e8c2c084968bc41888dd10bb1b1f3bf3508e2ca715838c0a66be873a65ce9d34a194071f128cbaca40771415f73699682"
        "c0c778b3ef49aa68e5ce34fff949c1841835aec59178f4babcc432d332dfbc281805e398c12367ec7a8a811f97f665d9ab2c"
        "7253000a3ac91c0e90e8e1ab738436f8b6a45b91450bf32ccc0a9f07e2a9f9264e62de1a353929ee6ab197051761d6da587f"
        "aef57f0dc6d29a"), out) && out == "Hello, hello, hello! A short string gets fixed Huffman codes." + Words(), "inflate several blocks with an empty stored one between");

    const std::string run = Hex("78daedc13101000000c2a0aceb5fc21a1e400100000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "000000000000000000000000000000af0679660b4d");  // 100000 x 'a': long matches at distance 1
    Check(ZlibInflate(run, out) && out == std::string(100000, 'a'), "inflate long matches");
    Check(ZlibInflate(run, out, 100000) && out.size() == 100000, "inflate up to exactly the limit");
    out = "stale";
    Check(!ZlibInflate(run, out, 99999) && out.empty(), "inflate stops past the limit");
    Check(!ZlibInflate(stored, out, 10), "the limit holds for stored blocks too");

    // Malformed streams.
    auto with = [](std::string s, size_t i, int x) { s[i] = (char)(s[i] ^ x); return s; };
    Check(!ZlibInflate("", out) && !ZlibInflate("\x78\x9c", out), "inflate refuses empty input");
    Check(!ZlibInflate(with(fixed, 0, 0x01), out), "inflate refuses a method other than deflate");
    Check(!ZlibInflate(with(fixed, 1, 0x01), out), "inflate refuses a bad header check");
    Check(!ZlibInflate("x " + fixed.substr(2), out), "inflate refuses a preset dictionary");  // 78 20: FDICT set, check valid
    Check(!ZlibInflate(with(fixed, fixed.size() - 1, 0x01), out), "inflate refuses a wrong Adler-32");
    Check(!ZlibInflate(with(dynamic, dynamic.size() / 2, 0x10), out), "inflate refuses corrupted data");
    Check(!ZlibInflate(with(stored, 4, 0x01), out), "inflate refuses a stored block whose length check fails");
    Check(!ZlibInflate(fixed + "x", out), "inflate refuses bytes after the stream");
    Check(!ZlibInflate(Hex("789c03020000000001"), out), "inflate refuses a distance before the start");
    Check(!ZlibInflate(Hex("789c0700000001"), out), "inflate refuses block type 3");

    bool truncated = true;
    for (size_t n = 0; n < dynamic.size(); n++) truncated &= !ZlibInflate(dynamic.substr(0, n), out);
    Check(truncated, "inflate refuses every truncation");

    // Every single-bit change either fails or still gives the original (unused padding bits).
    const std::string original[][2] = { { stored, "Stored block: zlib level 0 keeps these bytes as they are." }, { fixed, "Hello, hello, hello! A short string gets fixed Huffman codes." }, { dynamic, Words() } };
    bool robust = true;
    for (auto& o : original)
        for (size_t i = 0; i < o[0].size(); i++)
            for (int bit = 0; bit < 8; bit++)
                if (ZlibInflate(with(o[0], i, 1 << bit), out, 1 << 20) && out != o[1]) robust = false;
    Check(robust, "inflate survives every single-bit corruption");
}

static void Base64() {
    using Compat::Detail::Base64Decode;
    std::string out;
    Check(Base64Decode("Zm9vYmFy", out) && out == "foobar", "base64 without padding");
    Check(Base64Decode("Zm9vYg", out) && out == "foob" && Base64Decode("Zm9vYg==", out) && out == "foob", "base64 with and without padding");
    Check(Base64Decode("Zm9vYmE=", out) && out == "fooba", "base64 with one pad");
    Check(Base64Decode("-_8", out) && out == "\xfb\xff" && Base64Decode("+/8=", out) && out == "\xfb\xff", "base64 URL-safe and standard alphabets");
    Check(!Base64Decode("+_8", out), "base64 refuses mixed alphabets");
    Check(!Base64Decode("Zm9v*mFy", out) && !Base64Decode("Zm9v YmFy", out), "base64 refuses other characters");
    Check(!Base64Decode("Zm9vY", out) && !Base64Decode("Zg=", out) && !Base64Decode("Zg===", out) && !Base64Decode("Zm9vYmFy=", out), "base64 refuses bad lengths and padding");
}

static void FullFile() {
    using Compat::OpenCodexSwitcherFile;
    const std::string file = Hex("4353574601c46f0a10c68841d4b6be35387eb08fc816a6f48029901a3f3fb8a016417a2e32e851dedb9f41625ea028f3f28b"
        "c6a88844276477c99e6efee06d233e06410419f5f9c860f39439245478bc9f64c94079a9c006f79772ee67e6fe17");  // magic, version 1, salt, nonce, ciphertext||tag
    std::string json;
    Check(OpenCodexSwitcherFile(file, json) && json == "{\"version\":1,\"accounts\":[]}", "open a Codex Switcher file");

    auto with = [](std::string s, size_t i, int x) { s[i] = (char)(s[i] ^ x); return s; };
    auto refused = [&](const std::string& f) { json = "stale"; return !OpenCodexSwitcherFile(f, json) && json.empty(); };
    Check(refused(with(file, file.size() - 1, 0x01)), "file: refuses a flipped tag bit");
    Check(refused(with(file, 45, 0x01)), "file: refuses a flipped ciphertext bit");
    Check(refused(with(file, 5, 0x01)), "file: refuses a changed salt");
    Check(refused(with(file, 21, 0x01)), "file: refuses a changed nonce");
    Check(refused(file.substr(0, file.size() - 1)), "file: refuses a truncated file");
    Check(refused(file.substr(0, 45 + 15)) && refused(file.substr(0, 45)) && refused(file.substr(0, 4)) && refused(""), "file: refuses a file without room for the tag");
    Check(refused(with(file, 3, 'F' ^ 'X')), "file: refuses another magic");
    Check(refused(with(file, 4, 1 ^ 2)), "file: refuses version 2");
    Check(refused(Hex("4353574601c46f0a10c68841d4b6be35387eb08fc816a6f48029901a3f3fb8a016417a2e32e851dedb9f41625ea028f3f28b"
        "c6a88844276477c99e6efee06d233e06410419f5f9c860f39439245478bd6e49518631be8871e9597f6250169a09")), "file: refuses a corrupted zlib stream inside");
    Check(OpenCodexSwitcherFile(Hex("4353574601c46f0a10c68841d4b6be35387eb08fc816a6f48029901a3f3fb8a016417a2e32e851dedb9f41625ea028b565e0"
        "80858286ef9b0f9fc85a29ce2395635013230ece14ac47b2bd39a01071cc4f5836a31e23f9317ac52026138b7a1819033118"
        "168753236b4a7b5b026b8cba945971934e7217d8ba0aecbe1f2335b436bd59a1beaeff9e1179a0227a46523a27fb4952f56b"
        "97b712b3bbe1217000f6b6967bf460920e96e1895cec689f1a07dc5311dfc513076007b1d53dd1d282e81a354550021c8aee"
        "be0aa179a8be9e030f0f273a98a5d17919f3360fbbfa2a0186611b43010c1a53c4960955149281c953f6298813efe5d1f03b"
        "c49224b9ea58fcb8961aa17440c390c4c5e19236a6ec40d8dbf9496059547e70778f31485cad829bae53e32967e181f99c7d"
        "543f66916a0cb780b83af65247a0f4b23f19181b6a212ac0b0b6e512f954ba608c2027b1812a5336749edc76685debbd7dd3"
        "1ddeda106faef24c86c599cfa9880c62d1669edbeb44edf1fc69613a2201b6ddbc09b6fb9b6980073c3e82d76390afcf1c5e"
        "47c22852b091031cb5c24bdb87ba9c7c11b4f162a29ed9802dd1a0f0e6e357c7c0c6551b1e609bfc55e3f86e20d508bd31ec"
        "af537d3ed70dbd336944e872a5896b885250a5405a05b0915a1b99926dcadbfa69e9ddce8313a8bb8bedb3690af5bbc4487a"
        "c6e39ddd1ab87b052a9ab8c158d74f69fa6dd140538d19959b5abe91befc2150d90e9b3316fc644cabba55bd75abbaef7a1b"
        "4d1f8045a91f1c17df0d2b267f9fa32c90c7f69c52e8492ec00735610e29cfa8833345ee3aff6d634a420becd0890ac89a9c"
        "4981900a4f912a3e9fc0852fdddc5f362cc09e9152715013d6fbcedf65b927e148c837d8b5833a7e61268a94fb3a860e17a9"
        "55c21b54ccac569d4e11e5b5172956a111b6806137a4b675d2a2ddb41e8e7390219e799ceffca333caf1c3544cff4e365c66"
        "7055bba0f198eb42b71851e5dff7f236c52c289fe9dc355d9f28eafd5fb21641d75c8579d4164206c65f35ec02696cf4745b"
        "a9a6d7ffe54f70cdad6a67b1a9c12e7370e332cbe18769c21d6061d58d3e435c23fd7fcbbb98aa68e3c8eb02618fc1b5b42c"
        "7a285f413209c60289a510aa4ceadb9c24e9293dd9c334ae69f4e5603483c586da70a846f2578531392ed31a171ccda0f3e0"
        "bca93097eb0e5a076cc60a285045e4505aa6ba25bc7f48779ad0ee5624c657748634f6756601c03241eb62031252f23ff46e"
        "52e4f30f237463bc5ea541c9efa29bc0080cf55d3291e12ca900f8e97800a71be8c3752d7e0f9224ff2a3bdc8678da17bcb5"
        "c7b229f9298c749e6f1b97c59ce8d6c0f907d732140cb9998555a32d1e22e541c92f8e743295b585e12080b1c8034b48557c"
        "9d1dc552bfa05d0b669a09e57b71ad44c155ad7ec666de540c43372f7a41bda0bea3c51c5f62ee5998d77ca2044270c9393c"
        "928d3b837b0c107c8d7c634e217f0b6dac453896277736551eb335051d53d036fec0295fb6b07435086251151ebe6fe03850"
        "9408a73e9336bb875705149273f392aba49449af11ad4df66477792105ae4ab31df65ac752f122e4b75d92d703cabe04c2a7"
        "076f77f2f9b4a393767ae311755cf50917ad7399e2293c2cffa220e9c98630da13b1765495fed030322058c95a81965c7223"
        "f607d6fe40742ec5da7125859d2476c68971ea804f3e2bf80e28504ce2e771dff6a571630e1282eae3d6d3c76bf9b1bd906a"
        "04944b7504834fdbd1730ba5633d007384716ca85d7eb8d20e0cb2647a0cceee727bbdc4fc9973aa43622d52db621143d5da"
        "12231481cb48b45956c25af17b217931a2c818bec167ead6baabb0dacdf57f99cafccc993308d4b69f65a519ba5ad72c0335"
        "2c032f169ebeea0a805c6c16b6a2d96f32fb8ccfffd9f1a0a5be580f30c8a61c0534355da4f567cf1276cea95334b1c2bdd3"
        "892ec7882c70de45175ba502adfc35e13eb52348c13322041f49ea15764fcc92416be62a0cfaed919d37886568d3da7d4d51"
        "635276f37c1823fd75bde8355d9f513792d8ab135a1ff7c74d7f4dee02a9cfb129a5252aedf945279b17c1a46bfb34028e09"
        "0c2a66fe223fff20d3e40a6aa22bef4366ca6a1412ea2784ff3cc4bd9af47a47e83a30faa8d2d4b77b1f9b9f3e0508906eb7"
        "2510cc46db5a910e9bcb1f417254d02312dcc06a5888f28705bc0b532d84a6b684accc14f9009aee1366a943abd695e39cd2"
        "de4eeb191c2111ee3e1b877458601cc4dbbdce00765edb625266a039f683ad233dbbf229da4507e446d3cb7eddaa0a7bd24a"
        "354036ce1cfce6705222603cc8dd379c6462070bafcf172a055b6131e72abd77397dda4f0c0b54fccbe509605bfe50ca7d5d"
        "4d49ff4bf86b9dd1fbd6f5c8a54632dad058795c52bc11aaab4b45ddf1044487d74239196f46277803c0c5ba98f270a62943"
        "24f6e7528d4f3e06299e1801834498058073bcd37c0b8f552c560b04f7c429121692ae21b65cd783c9c4f7155e7bcf3d76d6"
        "73e087992a5e296a08cc269034a943ed0c3bafa918165c77243b82ea6ccf21075d22fb76995c2d97a4eae897fdbd69378728"
        "9ee6768f3beb18fdfe0eef7ee3e837b0ffa81327964d4cd98b1489e91e5e82b8bbf11d73d9ad3fd50c340382b0040bc05f79"
        "9393eac3b4dc8633f94a1fd684b86fdbe956b56ce4c4f529a89031a08154cf5c33c68b8c8a7cf62f0ff4e38ad39db4c9e261"
        "a9419c6e98048756cdd5cd9fb126721adb64d04059457e39d3eb29b554225b2697d278d881e2e840370c418f5025df0af786"
        "a3da2c751483eddd4ddf4322103a0626d618be1316b434363a165ff86d4f2890de79291e5993b6ea74e910623048039a39a1"
        "1956c7ad1f2a909c272f41583188bc54b806af8adee954e2bab810ef69a13c3c6c3f7c8f599ca19a27088dee632123085d34"
        "9ddb8205ed1747498f5b7d2829f83dc83c866b1b1a060eab89ada7ed2ed5"), json) && json.size() == 2u << 20, "file: JSON of exactly 2 MB");
    Check(refused(Hex("4353574601c46f0a10c68841d4b6be35387eb08fc816a6f48029901a3f3fb8a016417a2e32e851dedb9f41625ea028b565e0"
        "80858286ef9b0fb0657a62b8259dd751cbe90ece240c01f28539a01071cc4f5836a31e23f9317ac52026138b7a1819033118"
        "168753236b4a7b5b026b8cba945971934e7217d8ba0aecbe1f2335b436bd59a1beaeff9e1179a0227a46523a27fb4952f56b"
        "97b712b3bbe1217000f6b6967bf460920e96e1895cec689f1a07dc5311dfc513076007b1d53dd1d282e81a354550021c8aee"
        "be0aa179a8be9e030f0f273a98a5d17919f3360fbbfa2a0186611b43010c1a53c4960955149281c953f6298813efe5d1f03b"
        "c49224b9ea58fcb8961aa17440c390c4c5e19236a6ec40d8dbf9496059547e70778f31485cad829bae53e32967e181f99c7d"
        "543f66916a0cb780b83af65247a0f4b23f19181b6a212ac0b0b6e512f954ba608c2027b1812a5336749edc76685debbd7dd3"
        "1ddeda106faef24c86c599cfa9880c62d1669edbeb44edf1fc69613a2201b6ddbc09b6fb9b6980073c3e82d76390afcf1c5e"
        "47c22852b091031cb5c24bdb87ba9c7c11b4f162a29ed9802dd1a0f0e6e357c7c0c6551b1e609bfc55e3f86e20d508bd31ec"
        "af537d3ed70dbd336944e872a5896b885250a5405a05b0915a1b99926dcadbfa69e9ddce8313a8bb8bedb3690af5bbc4487a"
        "c6e39ddd1ab87b052a9ab8c158d74f69fa6dd140538d19959b5abe91befc2150d90e9b3316fc644cabba55bd75abbaef7a1b"
        "4d1f8045a91f1c17df0d2b267f9fa32c90c7f69c52e8492ec00735610e29cfa8833345ee3aff6d634a420becd0890ac89a9c"
        "4981900a4f912a3e9fc0852fdddc5f362cc09e9152715013d6fbcedf65b927e148c837d8b5833a7e61268a94fb3a860e17a9"
        "55c21b54ccac569d4e11e5b5172956a111b6806137a4b675d2a2ddb41e8e7390219e799ceffca333caf1c3544cff4e365c66"
        "7055bba0f198eb42b71851e5dff7f236c52c289fe9dc355d9f28eafd5fb21641d75c8579d4164206c65f35ec02696cf4745b"
        "a9a6d7ffe54f70cdad6a67b1a9c12e7370e332cbe18769c21d6061d58d3e435c23fd7fcbbb98aa68e3c8eb02618fc1b5b42c"
        "7a285f413209c60289a510aa4ceadb9c24e9293dd9c334ae69f4e5603483c586da70a846f2578531392ed31a171ccda0f3e0"
        "bca93097eb0e5a076cc60a285045e4505aa6ba25bc7f48779ad0ee5624c657748634f6756601c03241eb62031252f23ff46e"
        "52e4f30f237463bc5ea541c9efa29bc0080cf55d3291e12ca900f8e97800a71be8c3752d7e0f9224ff2a3bdc8678da17bcb5"
        "c7b229f9298c749e6f1b97c59ce8d6c0f907d732140cb9998555a32d1e22e541c92f8e743295b585e12080b1c8034b48557c"
        "9d1dc552bfa05d0b669a09e57b71ad44c155ad7ec666de540c43372f7a41bda0bea3c51c5f62ee5998d77ca2044270c9393c"
        "928d3b837b0c107c8d7c634e217f0b6dac453896277736551eb335051d53d036fec0295fb6b07435086251151ebe6fe03850"
        "9408a73e9336bb875705149273f392aba49449af11ad4df66477792105ae4ab31df65ac752f122e4b75d92d703cabe04c2a7"
        "076f77f2f9b4a393767ae311755cf50917ad7399e2293c2cffa220e9c98630da13b1765495fed030322058c95a81965c7223"
        "f607d6fe40742ec5da7125859d2476c68971ea804f3e2bf80e28504ce2e771dff6a571630e1282eae3d6d3c76bf9b1bd906a"
        "04944b7504834fdbd1730ba5633d007384716ca85d7eb8d20e0cb2647a0cceee727bbdc4fc9973aa43622d52db621143d5da"
        "12231481cb48b45956c25af17b217931a2c818bec167ead6baabb0dacdf57f99cafccc993308d4b69f65a519ba5ad72c0335"
        "2c032f169ebeea0a805c6c16b6a2d96f32fb8ccfffd9f1a0a5be580f30c8a61c0534355da4f567cf1276cea95334b1c2bdd3"
        "892ec7882c70de45175ba502adfc35e13eb52348c13322041f49ea15764fcc92416be62a0cfaed919d37886568d3da7d4d51"
        "635276f37c1823fd75bde8355d9f513792d8ab135a1ff7c74d7f4dee02a9cfb129a5252aedf945279b17c1a46bfb34028e09"
        "0c2a66fe223fff20d3e40a6aa22bef4366ca6a1412ea2784ff3cc4bd9af47a47e83a30faa8d2d4b77b1f9b9f3e0508906eb7"
        "2510cc46db5a910e9bcb1f417254d02312dcc06a5888f28705bc0b532d84a6b684accc14f9009aee1366a943abd695e39cd2"
        "de4eeb191c2111ee3e1b877458601cc4dbbdce00765edb625266a039f683ad233dbbf229da4507e446d3cb7eddaa0a7bd24a"
        "354036ce1cfce6705222603cc8dd379c6462070bafcf172a055b6131e72abd77397dda4f0c0b54fccbe509605bfe50ca7d5d"
        "4d49ff4bf86b9dd1fbd6f5c8a54632dad058795c52bc11aaab4b45ddf1044487d74239196f46277803c0c5ba98f270a62943"
        "24f6e7528d4f3e06299e1801834498058073bcd37c0b8f552c560b04f7c429121692ae21b65cd783c9c4f7155e7bcf3d76d6"
        "73e087992a5e296a08cc269034a943ed0c3bafa918165c77243b82ea6ccf21075d22fb76995c2d97a4eae897fdbd69378728"
        "9ee6768f3beb18fdfe0eef7ee3e837b0ffa81327964d4cd98b1489e91e5e82b8bbf11d73d9ad3fd50c340382b0040bc05f79"
        "9393eac3b4dc8633f94a1fd684b86fdbe956b56ce4c4f529a89031a08154cf5c33c68b8c8a7cf62f0ff4e38ad39db4c9e261"
        "a9419c6e98048756cdd5cd9fb126721adb64d04059457e39d3eb29b554225b2697d278d881e2e840370c418f5025df0af786"
        "a3da2c751483eddd4ddf4322103a0626d618be1316b434363a165ff86d4f2890de79291e5993b6ea74e910623048039a39a1"
        "1956c7ad1f2a909c272f41583188bc54b806af8adee954e2bab810ef69a13c3c6c3f7c8f599ca19a27088dee632123085d34"
        "9ddb8205ed1707eee1a2bf0b2998b9baa908fd19999c6d025fe32a8546ed")), "file: refuses JSON over 2 MB");
    Check(refused(file + std::string((8u << 20) - file.size() + 1, '\0')), "file: refuses more than 8 MB");
}

static void SlimText() {
    using Compat::OpenCodexSwitcherText;
    const std::string want = "{\"v\":1,\"a\":\"Работа\",\"c\":[{\"n\":\"Работа\",\"t\":0,\"k\":\"sk-test-10\"},{\"n\":\"Home\",\"t\":1,\"r\":\"rt_70\"}]}";
    const std::string url = "css1.eNqrVipTsjLUUUpUslK6sODChgsbL-y72HRhg5KOUrKSVXS1Uh6mRImSlYGOUjZQojhbtyS1uETX0ECpVgei1iM_NxWiBmh"
        "qEVCgqCTeHCgdWwsAF1MnNg";  // what Codex Switcher writes: URL-safe, no padding
    const std::string standard = "css1.eNqrVipTsjLUUUpUslK6sODChgsbL+y72HRhg5KOUrKSVXS1Uh6mRImSlYGOUjZQojhbtyS1uETX0ECpVgei1iM/NxWiBmh"
        "qEVCgqCTeHCgdWwsAF1MnNg==";
    std::string json, unpadded = standard;
    while (unpadded.back() == '=') unpadded.pop_back();
    Check(OpenCodexSwitcherText(url, json) && json == want, "slim text, URL-safe without padding");
    Check(OpenCodexSwitcherText(standard, json) && json == want, "slim text, standard with padding");
    Check(OpenCodexSwitcherText(unpadded, json) && json == want, "slim text, standard without padding");
    std::string spaced = "  " + url.substr(5, 10) + "\r\n" + url.substr(15, 7) + "\t " + url.substr(22) + "\n";
    Check(OpenCodexSwitcherText(spaced, json) && json == want, "slim text without the prefix, with whitespace");
    Check(OpenCodexSwitcherText(url.substr(0, 3) + "\n" + url.substr(3), json) && json == want, "slim text with a line break in the prefix");

    auto refused = [&](const std::string& t) { json = "stale"; return !OpenCodexSwitcherText(t, json) && json.empty(); };
    Check(refused("") && refused("css1.") && refused(" \n"), "slim text: refuses empty input");
    Check(refused(url.substr(0, 20) + "*" + url.substr(21)), "slim text: refuses a character outside base64");
    Check(refused(url.substr(0, url.size() - 3)), "slim text: refuses a truncated text");
    Check(refused("css2." + url.substr(5)), "slim text: refuses another prefix");
    Check(refused("css1.eNqrVipTsjLUUUpUslK6sODChgsbL-y72HRhg5KOUrKSVXS1Uh6mRImSlYGOUjZQojhbtyS1uETX0ECpVgei1iM_NxWiBmh"
        "qEVCgqCTeHCgdWwsAF1MnJg"), "slim text: refuses a corrupted zlib stream");
    Check(refused("css1.eNrtwTEBAAAAwqDai28MH6AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAzgZQReF5"), "slim text: refuses JSON over 2 MB");
}

int main() {
    ChaCha();
    Poly();
    Aead();
    Pbkdf2();
    Inflate();
    Base64();
    FullFile();
    SlimText();
    if (failures) {
        std::printf("%d of %d checks failed\n", failures, checks);
        return 1;
    }
    std::printf("All %d checks passed\n", checks);
    return 0;
}
