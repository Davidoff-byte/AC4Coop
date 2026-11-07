"""Decode the MSVC Rich header: which toolchain (and build number) produced a PE.

Matching decompilation is only possible if the original compiler is available and its exact
build is known. The Rich header records it.

    python rich_header.py <exe> [<exe> ...]

Product IDs are the documented MSVC ones; the build number is the low 16 bits of the compid.
"""
import struct
import sys

PRODUCTS = {
    0: "Unknown", 1: "Import0", 2: "Linker510", 3: "Cvtomf510", 4: "Linker600",
    5: "Cvtomf600", 6: "Cvtres500", 7: "Utc11_Basic", 8: "Utc11_C", 9: "Utc12_Basic",
    10: "Utc12_C", 11: "Utc12_CPP", 12: "AliasObj60", 13: "VisualBasic60",
    14: "Masm613", 15: "Masm710", 16: "Linker511", 17: "Cvtomf511", 18: "Masm614",
    19: "Linker512", 20: "Cvtomf512", 21: "Utc12_C_Std", 22: "Utc12_CPP_Std",
    23: "Utc12_C_Book", 24: "Utc12_CPP_Book", 25: "Implib700", 26: "Cvtomf700",
    27: "Utc13_Basic", 28: "Utc13_C", 29: "Utc13_CPP", 30: "Linker610", 31: "Cvtomf610",
    32: "Linker601", 33: "Cvtomf601", 34: "Utc12_1_Basic", 35: "Utc12_1_C", 36: "Utc12_1_CPP",
    37: "Linker620", 38: "Cvtomf620", 39: "AliasObj70", 40: "Linker621", 41: "Cvtomf621",
    42: "Masm615", 43: "Utc13_LTCG_C", 44: "Utc13_LTCG_CPP", 45: "Masm620",
    46: "ILAsm100", 47: "Utc12_2_Basic", 48: "Utc12_2_C", 49: "Utc12_2_CPP",
    50: "Utc12_2_C_Std", 51: "Utc12_2_CPP_Std", 52: "Utc12_2_C_Book", 53: "Utc12_2_CPP_Book",
    54: "Implib622", 55: "Cvtomf622", 56: "Cvtres501", 57: "Utc13_C_Std", 58: "Utc13_CPP_Std",
    59: "Cvtpgd1300", 60: "Linker622", 61: "Linker700", 62: "Export622", 63: "Export700",
    64: "Masm700", 65: "Utc13_POGO_I_C", 66: "Utc13_POGO_I_CPP", 67: "Utc13_POGO_O_C",
    68: "Utc13_POGO_O_CPP", 69: "Cvtres700", 70: "Cvtres710p", 71: "Linker710p",
    72: "Cvtomf710p", 73: "Export710p", 74: "Implib710p", 75: "Masm710p", 76: "Utc1310p_C",
    77: "Utc1310p_CPP", 78: "Utc1310p_C_Std", 79: "Utc1310p_CPP_Std", 80: "Utc1310p_LTCG_C",
    81: "Utc1310p_LTCG_CPP", 82: "Utc1310p_POGO_I_C", 83: "Utc1310p_POGO_I_CPP",
    84: "Utc1310p_POGO_O_C", 85: "Utc1310p_POGO_O_CPP", 86: "Linker624", 87: "Cvtomf624",
    88: "Export624", 89: "Implib624", 90: "Linker710", 91: "Cvtomf710", 92: "Export710",
    93: "Implib710", 94: "Cvtres710", 95: "Utc1310_C", 96: "Utc1310_CPP", 97: "Utc1310_C_Std",
    98: "Utc1310_CPP_Std", 99: "Utc1310_LTCG_C", 100: "Utc1310_LTCG_CPP",
    101: "Utc1310_POGO_I_C", 102: "Utc1310_POGO_I_CPP", 103: "Utc1310_POGO_O_C",
    104: "Utc1310_POGO_O_CPP", 105: "AliasObj710", 106: "AliasObj710p", 107: "Cvtpgd1310",
    108: "Cvtpgd1310p", 109: "Utc1400_C", 110: "Utc1400_CPP", 111: "Utc1400_C_Std",
    112: "Utc1400_CPP_Std", 113: "Utc1400_LTCG_C", 114: "Utc1400_LTCG_CPP",
    115: "Utc1400_POGO_I_C", 116: "Utc1400_POGO_I_CPP", 117: "Utc1400_POGO_O_C",
    118: "Utc1400_POGO_O_CPP", 119: "Cvtpgd1400", 120: "Linker800", 121: "Cvtomf800",
    122: "Export800", 123: "Implib800", 124: "Cvtres800", 125: "Masm800", 126: "AliasObj800",
    127: "PhoenixPrerelease", 128: "Utc1400_CVTCIL_C", 129: "Utc1400_CVTCIL_CPP",
    130: "Utc1400_LTCG_MSIL", 131: "Utc1500_C", 132: "Utc1500_CPP", 133: "Utc1500_C_Std",
    134: "Utc1500_CPP_Std", 135: "Utc1500_CVTCIL_C", 136: "Utc1500_CVTCIL_CPP",
    137: "Utc1500_LTCG_C", 138: "Utc1500_LTCG_CPP", 139: "Utc1500_POGO_I_C",
    140: "Utc1500_POGO_I_CPP", 141: "Utc1500_POGO_O_C", 142: "Utc1500_POGO_O_CPP",
    143: "Cvtpgd1500", 144: "Linker900", 145: "Export900", 146: "Implib900", 147: "Cvtres900",
    148: "Masm900", 149: "AliasObj900", 150: "Resource900", 151: "AliasObj1000",
    152: "Cvtres1000", 153: "Export1000", 154: "Implib1000", 155: "Linker1000", 156: "Masm1000",
    170: "Utc1600_C", 171: "Utc1600_CPP", 172: "Utc1600_CVTCIL_C", 173: "Utc1600_CVTCIL_CPP",
    174: "Utc1600_LTCG_C", 175: "Utc1600_LTCG_CPP", 176: "Utc1600_POGO_I_C",
    177: "Utc1600_POGO_I_CPP", 178: "Utc1600_POGO_O_C", 179: "Utc1600_POGO_O_CPP",
    180: "AliasObj1010", 181: "Cvtres1010", 182: "Export1010", 183: "Implib1010",
    184: "Linker1010", 185: "Masm1010", 199: "Utc1700_C", 200: "Utc1700_CPP",
    201: "Utc1700_CVTCIL_C", 202: "Utc1700_CVTCIL_CPP", 203: "Utc1700_LTCG_C",
    204: "Utc1700_LTCG_CPP", 205: "Utc1700_POGO_I_C", 206: "Utc1700_POGO_I_CPP",
    207: "Utc1700_POGO_O_C", 208: "Utc1700_POGO_O_CPP", 209: "AliasObj1100",
    210: "Cvtres1100", 211: "Export1100", 212: "Implib1100", 213: "Linker1100", 214: "Masm1100",
    215: "Utc1800_C", 216: "Utc1800_CPP", 217: "Utc1800_CVTCIL_C", 218: "Utc1800_CVTCIL_CPP",
    219: "Utc1800_LTCG_C", 220: "Utc1800_LTCG_CPP", 221: "Utc1800_POGO_I_C",
    222: "Utc1800_POGO_I_CPP", 223: "Utc1800_POGO_O_C", 224: "Utc1800_POGO_O_CPP",
    225: "AliasObj1200", 226: "Cvtres1200", 227: "Export1200", 228: "Implib1200",
    229: "Linker1200", 230: "Masm1200", 231: "Utc1900_C", 232: "Utc1900_CPP",
    233: "Utc1900_CVTCIL_C", 234: "Utc1900_CVTCIL_CPP", 235: "Utc1900_LTCG_C",
    236: "Utc1900_LTCG_CPP", 237: "Utc1900_POGO_I_C", 238: "Utc1900_POGO_I_CPP",
    239: "Utc1900_POGO_O_C", 240: "Utc1900_POGO_O_CPP",
}


def rich(path):
    data = open(path, "rb").read()
    if data[:2] != b"MZ":
        return None
    off = data.find(b"Rich", 0, 0x1000)
    if off < 0:
        return None
    key = struct.unpack_from("<I", data, off + 4)[0]
    # entries run backwards from "Rich"; find the encoded DanS marker
    start = None
    for i in range(off - 4, 0, -4):
        if struct.unpack_from("<I", data, i)[0] ^ key == 0x536E6144:  # 'DanS'
            start = i
            break
    if start is None:
        return None
    entries = []
    p = start + 16
    while p + 8 <= off:
        compid, count = struct.unpack_from("<II", data, p)
        compid ^= key
        count ^= key
        prodid = compid >> 16
        build = compid & 0xFFFF
        entries.append((prodid, build, count))
        p += 8
    return entries


def main(argv):
    for path in argv[1:]:
        print(f"== {path}")
        ents = rich(path)
        if not ents:
            print("   no Rich header")
            continue
        for prodid, build, count in sorted(ents, key=lambda e: -e[2]):
            name = PRODUCTS.get(prodid, f"prod_{prodid}")
            print(f"   {name:28} build {build:5d}   x{count}")
        builds = sorted({b for _, b, _ in ents if b})
        print(f"   distinct tool builds: {builds}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
