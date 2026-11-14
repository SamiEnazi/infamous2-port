#!/usr/bin/env python3
r"""ps3tool - one file, three jobs, no RPCS3.

    ps3tool decrypt <in> <out>   EBOOT.BIN / *.sprx (SELF) -> decrypted ELF/PRX
    ps3tool pkg     <in> <out>   game update .pkg          -> folder (PARAM.SFO, USRDIR/...)
    ps3tool pup     <in> <out>   PS3UPDAT.PUP              -> dev_flash folder (sys/external/...)

Exit 0 on success. On failure: a non-zero exit and one plain-English line on stderr.
Only <out> is written; <in> is never modified and nothing is written beside it.
An input that is already a plain ELF is copied to <out> by "decrypt".

The public PS3 keys are embedded, the same constants RPCS3's key_vault.cpp uses.
An external key file can override or add keys: PS3TOOL_KEYS,
%APPDATA%\ps3tool\keys.txt, ~/.config/ps3tool/keys.txt, or ps3tool.keys next to
the program. The file format is described below. AES uses pycryptodome when it
is installed and a pure-Python fallback otherwise (set PS3TOOL_NO_CRYPTO=1 to
force the fallback). No network, no prompts, no window.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import os
import posixpath
import re
import struct
import sys
import tarfile
import zlib

try:
    from Crypto.Cipher import AES as _CryptoAES
except Exception:
    _CryptoAES = None
if os.environ.get("PS3TOOL_NO_CRYPTO") == "1":
    _CryptoAES = None


class Ps3ToolError(Exception):
    pass


# ---------------------------------------------------------------------------
# Keys: the public constants RPCS3's key_vault.cpp uses, embedded here too
# ---------------------------------------------------------------------------
# These are public PS3 constants, not per-user secrets. An external key file can
# override or add keys (PS3TOOL_KEYS, %APPDATA%\ps3tool\keys.txt,
# ~/.config/ps3tool/keys.txt, or ps3tool.keys next to the program). The file
# accepts simple name=hex lines:
#
#   APP_0010_ERK=...        APP_0010_RIV=...
#   NPDRM_0010_ERK=...      NPDRM_0010_RIV=...
#   NP_KLIC_FREE=...        NP_KLIC_KEY=...        RAP_KEY=...
#   SCEPKG_ERK=...          SCEPKG_RIV=...
#   PS3_GPKG_KEY=...        PSP_PKG_KEY=...
#
# and scetool-style [section] blocks (type=SELF, revision=, self_type=, erk=, riv=;
# type=PKG, erk=, riv=; [NP_klic_free] key=...; [NP_klic_key] key=...).
SELF_KEYS = {
    ('APP', 0x0000): ('95F50019E7A68E341FA72EFDF4D60ED376E25CF46BB48DFDD1F080259DC93F04', '4A0955D946DB70D691A640BB7FAECC4C'),
    ('APP', 0x0001): ('79481839C406A632BDB4AC093D73D99AE1587F24CE7E69192C1CD0010274A8AB', '6F0F25E1C8C4B7AE70DF968B04521DDA'),
    ('APP', 0x0002): ('4F89BE98DDD43CAD343F5BA6B1A133B0A971566F770484AAC20B5DD1DC9FA06A', '90C127A9B43BA9D8E89FE6529E25206F'),
    ('APP', 0x0003): ('C1E6A351FCED6A0636BFCB6801A0942DB7C28BDFC5E0A053A3F52F52FCE9754E', 'E0908163F457576440466ACAA443AE7C'),
    ('APP', 0x0004): ('838F5860CF97CDAD75B399CA44F4C214CDF951AC795298D71DF3C3B7E93AAEDA', '7FDBB2E924D182BB0D69844ADC4ECA5B'),
    ('APP', 0x0005): ('C109AB56593DE5BE8BA190578E7D8109346E86A11088B42C727E2B793FD64BDC', '15D3F191295C94B09B71EBDE088A187A'),
    ('APP', 0x0006): ('6DFD7AFB470D2B2C955AB22264B1FF3C67F180983B26C01615DE9F2ECCBE7F41', '24BD1C19D2A8286B8ACE39E4A37801C2'),
    ('APP', 0x0007): ('945B99C0E69CAF0558C588B95FF41B232660ECB017741F3218C12F9DFDEEDE55', '1D5EFBE7C5D34AD60F9FBC46A5977FCE'),
    ('APP', 0x0008): ('2C9E8969EC44DFB6A8771DC7F7FDFBCCAF329EC3EC070900CABB23742A9A6E13', '5A4CEFD5A9C3C093D0B9352376D19405'),
    ('APP', 0x0009): ('F69E4A2934F114D89F386CE766388366CDD210F1D8913E3B973257F1201D632B', 'F4D535069301EE888CC2A852DB654461'),
    ('APP', 0x000A): ('29805302E7C92F204009161CA93F776A072141A8C46A108E571C46D473A176A3', '5D1FAB844107676ABCDFC25EAEBCB633'),
    ('APP', 0x000B): ('A4C97402CC8A71BC7748661FE9CE7DF44DCE95D0D58938A59F47B9E9DBA7BFC3', 'E4792F2B9DB30CB8D1596077A13FB3B5'),
    ('APP', 0x000C): ('9814EFFF67B7074D1B263BF85BDC8576CE9DEC914123971B169472A1BC2387FA', 'D43B1FA8BE15714B3078C23908BB2BCA'),
    ('APP', 0x000D): ('03B4C421E0C0DE708C0F0B71C24E3EE04306AE7383D8C5621394CCB99FF7A194', '5ADB9EAFE897B54CB1060D6885BE22CF'),
    ('APP', 0x000E): ('39A870173C226EB8A3EEE9CA6FB675E82039B2D0CCB22653BFCE4DB013BAEA03', '90266C98CBAA06C1BF145FF760EA1B45'),
    ('APP', 0x000F): ('FD52DFA7C6EEF5679628D12E267AA863B9365E6DB95470949CFD235B3FCA0F3B', '64F50296CF8CF49CD7C643572887DA0B'),
    ('APP', 0x0010): ('A5E51AD8F32FFBDE808972ACEE46397F2D3FE6BC823C8218EF875EE3A9B0584F', '7A203D5112F799979DF0E1B8B5B52AA4'),
    ('APP', 0x0011): ('0F8EAB8884A51D092D7250597388E3B8B75444AC138B9D36E5C7C5B8C3DF18FD', '97AF39C383E7EF1C98FA447C597EA8FE'),
    ('APP', 0x0013): ('DBF62D76FC81C8AC92372A9D631DDC9219F152C59C4B20BFF8F96B64AB065E94', 'CB5DD4BE8CF115FFB25801BC6086E729'),
    ('APP', 0x0014): ('491B0D72BB21ED115950379F4564CE784A4BFAABB00E8CB71294B192B7B9F88E', 'F98843588FED8B0E62D7DDCB6F0CECF4'),
    ('APP', 0x0016): ('A106692224F1E91E1C4EBAD4A25FBFF66B4B13E88D878E8CD072F23CD1C5BF7C', '62773C70BD749269C0AFD1F12E73909E'),
    ('APP', 0x0017): ('4E104DCE09BA878C75DA98D0B1636F0E5F058328D81419E2A3D22AB0256FDF46', '954A86C4629E116532304A740862EF85'),
    ('APP', 0x0018): ('1F876AB252DDBCB70E74DC4A20CD8ED51E330E62490E652F862877E8D8D0F997', 'BF8D6B1887FA88E6D85C2EDB2FBEC147'),
    ('APP', 0x0019): ('3236B9937174DF1DC12EC2DD8A318A0EA4D3ECDEA5DFB4AC1B8278447000C297', '6153DEE781B8ADDC6A439498B816DC46'),
    ('APP', 0x001A): ('5EFD1E9961462794E3B9EF2A4D0C1F46F642AAE053B5025504130590E66F19C9', '1AC8FA3B3C90F8FDE639515F91B58327'),
    ('APP', 0x001B): ('66637570D1DEC098467DB207BAEA786861964D0964D4DBAF89E76F46955D181B', '9F7B5713A5ED59F6B35CD8F8A165D4B8'),
    ('APP', 0x001C): ('CFF025375BA0079226BE01F4A31F346D79F62CFB643CA910E16CF60BD9092752', 'FD40664E2EBBA01BF359B0DCDF543DA4'),
    ('APP', 0x001D): ('D202174EB65A62048F3674B59EF6FE72E1872962F3E1CD658DE8D7AF71DA1F3E', 'ACB9945914EBB7B9A31ECE320AE09F2D'),
    ('NPDRM', 0x0001): ('F9EDD0301F770FABBA8863D9897F0FEA6551B09431F61312654E28F43533EA6B', 'A551CCB4A42C37A734A2B4F9657D5540'),
    ('NPDRM', 0x0002): ('8E737230C80E66AD0162EDDD32F1F774EE5E4E187449F19079437A508FCF9C86', '7AAECC60AD12AED90C348D8C11D2BED5'),
    ('NPDRM', 0x0003): ('1B715B0C3E8DC4C1A5772EBA9C5D34F7CCFE5B82025D453F3167566497239664', 'E31E206FBB8AEA27FAB0D9A2FFB6B62F'),
    ('NPDRM', 0x0004): ('BB4DBF66B744A33934172D9F8379A7A5EA74CB0F559BB95D0E7AECE91702B706', 'ADF7B207A15AC601110E61DDFC210AF6'),
    ('NPDRM', 0x0006): ('8B4C52849765D2B5FA3D5628AFB17644D52B9FFEE235B4C0DB72A62867EAA020', '05719DF1B1D0306C03910ADDCE4AF887'),
    ('NPDRM', 0x0007): ('3946DFAA141718C7BE339A0D6C26301C76B568AEBC5CD52652F2E2E0297437C3', 'E4897BE553AE025CDCBF2B15D1C9234E'),
    ('NPDRM', 0x0009): ('0786F4B0CA5937F515BDCE188F569B2EF3109A4DA0780A7AA07BD89C3350810A', '04AD3C2F122A3B35E804850CAD142C6D'),
    ('NPDRM', 0x000A): ('03C21AD78FBB6A3D425E9AAB1298F9FD70E29FD4E6E3A3C151205DA50C413DE4', '0A99D4D4F8301A88052D714AD2FB565E'),
    ('NPDRM', 0x000C): ('357EBBEA265FAEC271182D571C6CD2F62CFA04D325588F213DB6B2E0ED166D92', 'D26E6DD2B74CD78E866E742E5571B84F'),
    ('NPDRM', 0x000D): ('337A51416105B56E40D7CAF1B954CDAF4E7645F28379904F35F27E81CA7B6957', '8405C88E042280DBD794EC7E22B74002'),
    ('NPDRM', 0x000F): ('135C098CBE6A3E037EBE9F2BB9B30218DDE8D68217346F9AD33203352FBB3291', '4070C898C2EAAD1634A288AA547A35A8'),
    ('NPDRM', 0x0010): ('4B3CD10F6A6AA7D99F9B3A660C35ADE08EF01C2C336B9E46D1BB5678B4261A61', 'C0F2AB86E6E0457552DB50D7219371C5'),
    ('NPDRM', 0x0013): ('265C93CF48562EC5D18773BEB7689B8AD10C5EB6D21421455DEBC4FB128CBF46', '8DEA5FF959682A9B98B688CEA1EF4A1D'),
    ('NPDRM', 0x0016): ('7910340483E419E55F0D33E4EA5410EEEC3AF47814667ECA2AA9D75602B14D4B', '4AD981431B98DFD39B6388EDAD742A8E'),
    ('NPDRM', 0x0019): ('FBDA75963FE690CFF35B7AA7B408CF631744EDEF5F7931A04D58FD6A921FFDB3', 'F72C1D80FFDA2E3BF085F4133E6D2805'),
    ('NPDRM', 0x001C): ('8103EA9DB790578219C4CEDF0592B43064A7D98B601B6C7BC45108C4047AA80F', '246F4B8328BE6A2D394EDE20479247C5'),
}

EMBEDDED_NAMED = {
    'np_klic_free': '72F990788F9CFF745725F08E4C128387',
    'np_klic_key': 'F2FBCA7A75B04EDC1390638CCDFDD1EE',
    'rap_key': '869F7745C13FD890CCF29188E3CC3EDF',
    'scepkg_erk': 'A97818BD193A67A16FE83A855E1BE9FB5640938D4DBCB2CB52C5A2F8B02B1031',
    'scepkg_riv': '4ACEF01224FBEEDF8245F8FF10211E6E',
    'ps3_gpkg_key': '2e7b71d7c9c9a14ea3221f188828b8f8',
    'psp_pkg_key': '07f2c68290b50d2c33818d709b60e62b',
}

RAP_PBOX = [0x0C, 0x03, 0x06, 0x04, 0x01, 0x0B, 0x0F, 0x08, 0x02, 0x07, 0x00, 0x05, 0x0A, 0x0E, 0x0D, 0x09]
RAP_E1 = [0xA9, 0x3E, 0x1F, 0xD6, 0x7C, 0x55, 0xA3, 0x29, 0xB7, 0x5F, 0xDD, 0xA6, 0x2A, 0x95, 0xC7, 0xA5]
RAP_E2 = [0x67, 0xD4, 0x5D, 0xA3, 0x29, 0x6D, 0x00, 0x6A, 0x4E, 0x7C, 0x53, 0x7B, 0xF5, 0x53, 0x8C, 0x74]


def find_key_file():
    env = os.environ.get('PS3TOOL_KEYS')
    if env:
        return env
    here = os.path.dirname(os.path.abspath(
        sys.executable if getattr(sys, 'frozen', False) else __file__))
    candidates = [
        os.path.join(here, 'ps3tool.keys'),
        os.path.join(here, 'keys.txt'),
    ]
    appdata = os.environ.get('APPDATA')
    if appdata:
        candidates += [os.path.join(appdata, 'ps3tool', 'keys.txt'),
                       os.path.join(appdata, 'ps3tool', 'ps3tool.keys')]
    local = os.environ.get('LOCALAPPDATA')
    if local:
        candidates += [os.path.join(local, 'ps3tool', 'keys.txt')]
    home = os.path.expanduser('~')
    candidates += [
        os.path.join(home, '.config', 'ps3tool', 'keys.txt'),
        os.path.join(home, '.ps3tool', 'keys.txt'),
        os.path.join(home, '.ps3tool', 'ps3tool.keys'),
    ]
    for p in candidates:
        if os.path.isfile(p):
            return p
    return None


class Keys:
    def __init__(self):
        # The public constants RPCS3 embeds; an external key file can override them.
        self.self_keys = {k: (bytes.fromhex(v[0]), bytes.fromhex(v[1]))
                          for k, v in SELF_KEYS.items()}
        self.named = {k: bytes.fromhex(v) for k, v in EMBEDDED_NAMED.items()}

    @classmethod
    def load(cls, path=None):
        k = cls()
        path = path or find_key_file()
        if not path:
            return k
        try:
            with open(path, encoding='utf-8', errors='replace') as f:
                text = f.read()
        except OSError as e:
            raise Ps3ToolError('cannot read the key file %s: %s' % (path, e))
        k.parse(text)
        return k

    @staticmethod
    def _hex(v):
        v = v.strip().replace(' ', '').replace(',', '')
        if v.lower().startswith('0x'):
            v = v[2:]
        try:
            return bytes.fromhex(v)
        except ValueError:
            raise Ps3ToolError('the key file has a value that is not hex')

    def _set_named(self, name, val):
        self.named[name.strip().lower()] = self._hex(val)

    def parse(self, text):
        blocks, cur = [], None
        for raw in text.splitlines():
            line = raw.split('#', 1)[0].split(';', 1)[0].strip()
            if not line:
                continue
            if line.startswith('[') and line.endswith(']'):
                cur = {'name': line[1:-1].strip()}
                blocks.append(cur)
                continue
            if '=' not in line:
                continue
            name, _, val = line.partition('=')
            name, val = name.strip(), val.strip()
            if cur is None:
                self._set_named(name, val)
            else:
                cur[name.lower()] = val
        for b in blocks:
            kind = (b.get('type') or b.get('category') or '').upper()
            ptype = (b.get('self_type') or b.get('program_type') or '').upper()
            if kind == 'SELF' and b.get('erk') and b.get('riv') and b.get('revision'):
                try:
                    rev = int(b['revision'], 16)
                except ValueError:
                    raise Ps3ToolError('the key file has a bad revision in [%s]' % b['name'])
                self.self_keys[(ptype, rev)] = (
                    self._hex(b['erk']), self._hex(b['riv']))
            elif kind == 'PKG' and b.get('erk') and b.get('riv'):
                self.named['scepkg_erk'] = self._hex(b['erk'])
                self.named['scepkg_riv'] = self._hex(b['riv'])
            elif b.get('key'):
                self._set_named(b['name'], b['key'])
        # APP_0010_ERK / NPDRM_0010_RIV style names
        for name, val in list(self.named.items()):
            m = re.match(r'^(app|npdrm)_([0-9a-f]{4})_(erk|riv)$', name)
            if not m:
                continue
            typ = m.group(1).upper()
            rev = int(m.group(2), 16)
            erk, riv = self.self_keys.get((typ, rev), (None, None))
            if m.group(3) == 'erk':
                erk = val
            else:
                riv = val
            self.self_keys[(typ, rev)] = (erk, riv)

    def self_key(self, type_name, revision):
        key = self.self_keys.get((type_name, revision))
        if not key or not key[0] or not key[1]:
            raise Ps3ToolError('the key file has no %s key for revision 0x%04X'
                               % (type_name, revision))
        if len(key[0]) != 32 or len(key[1]) != 16:
            raise Ps3ToolError('the key file\'s %s key for revision 0x%04X has the wrong length'
                               % (type_name, revision))
        return key

    def get(self, name, size=None):
        v = self.named.get(name.lower())
        if v is None:
            raise Ps3ToolError('the key file has no %s' % name)
        if size is not None and len(v) != size:
            raise Ps3ToolError('the key file\'s %s is %d bytes; expected %d'
                               % (name, len(v), size))
        return v


# ---------------------------------------------------------------------------
# AES (pycryptodome when present, pure Python otherwise)
# ---------------------------------------------------------------------------
def _make_sbox():
    sbox = [0] * 256
    p = q = 1
    while True:
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
        q ^= q << 1
        q ^= q << 2
        q ^= q << 4
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09
        x = q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4))
        sbox[p] = (x ^ 0x63) & 0xFF
        if p == 1:
            break
    sbox[0] = 0x63
    inv = [0] * 256
    for i, v in enumerate(sbox):
        inv[v] = i
    return sbox, inv


SBOX, INV_SBOX = _make_sbox()


def _xtime(a):
    return ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else (a << 1)


def _gmul(a, b):
    p = 0
    for _ in range(8):
        if b & 1:
            p ^= a
        hi = a & 0x80
        a = (a << 1) & 0xFF
        if hi:
            a ^= 0x1B
        b >>= 1
    return p


def _mix_column(s, c):
    i = 4 * c
    a0, a1, a2, a3 = s[i], s[i + 1], s[i + 2], s[i + 3]
    t = a0 ^ a1 ^ a2 ^ a3
    s[i] = a0 ^ t ^ _xtime(a0 ^ a1)
    s[i + 1] = a1 ^ t ^ _xtime(a1 ^ a2)
    s[i + 2] = a2 ^ t ^ _xtime(a2 ^ a3)
    s[i + 3] = a3 ^ t ^ _xtime(a3 ^ a0)


def _inv_mix_column(s, c):
    i = 4 * c
    a0, a1, a2, a3 = s[i], s[i + 1], s[i + 2], s[i + 3]
    s[i] = _gmul(a0, 14) ^ _gmul(a1, 11) ^ _gmul(a2, 13) ^ _gmul(a3, 9)
    s[i + 1] = _gmul(a0, 9) ^ _gmul(a1, 14) ^ _gmul(a2, 11) ^ _gmul(a3, 13)
    s[i + 2] = _gmul(a0, 13) ^ _gmul(a1, 9) ^ _gmul(a2, 14) ^ _gmul(a3, 11)
    s[i + 3] = _gmul(a0, 11) ^ _gmul(a1, 13) ^ _gmul(a2, 9) ^ _gmul(a3, 14)


def _shift_rows(s):
    s[1], s[5], s[9], s[13] = s[5], s[9], s[13], s[1]
    s[2], s[6], s[10], s[14] = s[10], s[14], s[2], s[6]
    s[3], s[7], s[11], s[15] = s[15], s[3], s[7], s[11]


def _inv_shift_rows(s):
    s[1], s[5], s[9], s[13] = s[13], s[1], s[5], s[9]
    s[2], s[6], s[10], s[14] = s[10], s[14], s[2], s[6]
    s[3], s[7], s[11], s[15] = s[7], s[11], s[15], s[3]


def _add_round_key(s, rk, rnd):
    off = rnd * 16
    for i in range(16):
        s[i] ^= rk[off + i]


class _AES:
    def __init__(self, key):
        nk = len(key) // 4
        if nk not in (4, 6, 8):
            raise Ps3ToolError('AES key must be 16, 24 or 32 bytes')
        self.nr = nk + 6
        w = [list(key[4 * i:4 * i + 4]) for i in range(nk)]
        rcon = 1
        for i in range(nk, 4 * (self.nr + 1)):
            temp = list(w[i - 1])
            if i % nk == 0:
                temp = temp[1:] + temp[:1]
                temp = [SBOX[b] for b in temp]
                temp[0] ^= rcon
                rcon = _xtime(rcon)
            elif nk > 6 and i % nk == 4:
                temp = [SBOX[b] for b in temp]
            w.append([w[i - nk][j] ^ temp[j] for j in range(4)])
        self.rk = [b for word in w for b in word]

    def encrypt_block(self, block):
        s = list(block)
        _add_round_key(s, self.rk, 0)
        for rnd in range(1, self.nr):
            s = [SBOX[b] for b in s]
            _shift_rows(s)
            for c in range(4):
                _mix_column(s, c)
            _add_round_key(s, self.rk, rnd)
        s = [SBOX[b] for b in s]
        _shift_rows(s)
        _add_round_key(s, self.rk, self.nr)
        return bytes(s)

    def decrypt_block(self, block):
        s = list(block)
        _add_round_key(s, self.rk, self.nr)
        for rnd in range(self.nr - 1, 0, -1):
            _inv_shift_rows(s)
            s = [INV_SBOX[b] for b in s]
            _add_round_key(s, self.rk, rnd)
            for c in range(4):
                _inv_mix_column(s, c)
        _inv_shift_rows(s)
        s = [INV_SBOX[b] for b in s]
        _add_round_key(s, self.rk, 0)
        return bytes(s)


def ecb_encrypt(key, data):
    if _CryptoAES is not None:
        return _CryptoAES.new(key, _CryptoAES.MODE_ECB).encrypt(data)
    aes = _AES(key)
    return b''.join(aes.encrypt_block(data[i:i + 16]) for i in range(0, len(data), 16))


def ecb_decrypt(key, data):
    if _CryptoAES is not None:
        return _CryptoAES.new(key, _CryptoAES.MODE_ECB).decrypt(data)
    aes = _AES(key)
    return b''.join(aes.decrypt_block(data[i:i + 16]) for i in range(0, len(data), 16))


def cbc_decrypt(key, iv, data):
    if _CryptoAES is not None:
        return _CryptoAES.new(key, _CryptoAES.MODE_CBC, iv).decrypt(data)
    aes = _AES(key)
    out = bytearray()
    prev = iv
    for i in range(0, len(data), 16):
        block = data[i:i + 16]
        dec = aes.decrypt_block(block)
        out += bytes(a ^ b for a, b in zip(dec, prev))
        prev = block
    return bytes(out)


def ctr_crypt(key, iv, data):
    if _CryptoAES is not None:
        return _CryptoAES.new(key, _CryptoAES.MODE_CTR, nonce=b'', initial_value=iv).decrypt(data)
    aes = _AES(key)
    out = bytearray()
    ctr = int.from_bytes(iv, 'big')
    for i in range(0, len(data), 16):
        ks = aes.encrypt_block(ctr.to_bytes(16, 'big'))
        chunk = data[i:i + 16]
        out += bytes(a ^ b for a, b in zip(chunk, ks))
        ctr = (ctr + 1) & ((1 << 128) - 1)
    return bytes(out)


# ---------------------------------------------------------------------------
# SELF -> ELF
# ---------------------------------------------------------------------------
KEY_REV_DEBUG = 0x8000
PROGRAM_TYPES = {1: 'LV0', 2: 'LV1', 3: 'LV2', 4: 'APP', 5: 'ISO', 6: 'LDR', 7: 'UNK7', 8: 'NPDRM'}
SECTION_FIELDS = ('data_off', 'data_size', 'type', 'prog_idx', 'hashed', 'sha1_idx',
                  'encrypted', 'key_idx', 'iv_idx', 'compressed')


def rap_to_rif(rap, rap_key):
    key = bytearray(ecb_decrypt(rap_key, rap))
    for _ in range(5):
        for i in range(16):
            p = RAP_PBOX[i]
            key[p] ^= RAP_E1[p]
        for i in range(15, 0, -1):
            p, pp = RAP_PBOX[i], RAP_PBOX[i - 1]
            key[p] ^= key[pp]
        o = 0
        for i in range(16):
            p = RAP_PBOX[i]
            kc = (key[p] - o) & 0xFF
            ec2 = RAP_E2[p]
            if o != 1 or kc != 0xFF:
                o = 1 if kc < ec2 else 0
                key[p] = (kc - ec2) & 0xFF
            elif kc == 0xFF:
                key[p] = (kc - ec2) & 0xFF
            else:
                key[p] = kc
    return bytes(key)


def find_rap(in_path, content_id):
    """The player's own .rap, if it is next to the SELF or in RPCS3's exdata folder."""
    d = os.path.dirname(os.path.abspath(in_path))
    candidates = [
        os.path.join(d, content_id + '.rap'),
        os.path.join(d, os.path.basename(in_path) + '.rap'),
    ]
    env = os.environ.get('PS3_RAP_DIR')
    if env:
        candidates.append(os.path.join(env, content_id + '.rap'))
    home = os.path.expanduser('~')
    candidates += [
        os.path.join(home, '.config', 'rpcs3', 'dev_hdd0', 'home', '00000001', 'exdata', content_id + '.rap'),
        os.path.join(home, '.var', 'app', 'net.rpcs3.RPCS3', 'config', 'rpcs3', 'dev_hdd0', 'home',
                     '00000001', 'exdata', content_id + '.rap'),
        os.path.join(home, 'Library', 'Application Support', 'rpcs3', 'dev_hdd0', 'home',
                     '00000001', 'exdata', content_id + '.rap'),
    ]
    appdata = os.environ.get('APPDATA')
    if appdata:
        candidates.append(os.path.join(appdata, 'rpcs3', 'dev_hdd0', 'home', '00000001', 'exdata',
                                       content_id + '.rap'))
    for p in candidates:
        try:
            if os.path.isfile(p):
                with open(p, 'rb') as f:
                    rap = f.read()
                if len(rap) == 16:
                    return rap
        except OSError:
            pass
    return None


def sce_metadata(d, off, header_len, info):
    """Section table and data keys from a decrypted 0x40-byte SCE metadata info."""
    key, key_pad, iv, iv_pad = info[0:16], info[16:32], info[32:48], info[48:64]
    if key_pad != bytes(16) or iv_pad != bytes(16):
        raise Ps3ToolError('metadata info did not decrypt (wrong key, or the wrong .rap for this copy)')
    if header_len > len(d) or header_len < off + 0x40 + 0x20:
        raise Ps3ToolError('bad SCE header length')
    hdrs = ctr_crypt(key, iv, d[off + 0x40:header_len])
    _sig, _u1, nsec, nkeys, _opt = struct.unpack_from('>QIIII', hdrs, 0)
    if not (0 < nsec < 256 and 0 < nkeys < 1024) or 0x20 + nsec * 0x30 + nkeys * 0x10 > len(hdrs):
        raise Ps3ToolError('implausible metadata (%d sections, %d keys)' % (nsec, nkeys))
    sections = [dict(zip(SECTION_FIELDS, struct.unpack_from('>QQIIIIIIII', hdrs, 0x20 + i * 0x30)))
                for i in range(nsec)]
    ko = 0x20 + nsec * 0x30
    dkeys = [hdrs[ko + i * 0x10:ko + (i + 1) * 0x10] for i in range(nkeys)]
    return sections, dkeys


class Self:
    def __init__(self, data, path=''):
        self.d = data
        self.path = path
        if len(data) < 0x70 or data[:4] != b'SCE\0':
            raise Ps3ToolError('not a SELF file (no SCE magic)')
        (_, self.version, self.key_revision, self.header_type, self.meta_off,
         self.header_len, self.data_len) = struct.unpack_from('>4sIHHIQQ', data, 0)
        if self.header_type != 1:
            raise Ps3ToolError('SCE header type %d is not a SELF' % self.header_type)
        (_ext, self.app_info_off, self.elf_off, self.phdr_off, self.shdr_off,
         _seg, _ver, self.ctrl_off, self.ctrl_size, _pad) = struct.unpack_from('>10Q', data, 0x20)
        self._need(self.app_info_off, 0x18, 'program info')
        (self.auth_id, self.vendor_id, self.program_type,
         self.app_version) = struct.unpack_from('>QIIQ', data, self.app_info_off)

        self._need(self.elf_off, 0x40, 'ELF header')
        self.eh = data[self.elf_off:self.elf_off + 0x40]
        if self.eh[:4] != b'\x7fELF':
            raise Ps3ToolError('the embedded ELF header is missing')
        if self.eh[4] != 2:
            raise Ps3ToolError('a 32-bit ELF (an SPU image?): only 64-bit PPU ELFs are handled')
        self.e_entry, self.e_phoff, self.e_shoff = struct.unpack_from('>QQQ', self.eh, 0x18)
        self.e_phentsize, self.e_phnum, self.e_shentsize, self.e_shnum = \
            struct.unpack_from('>HHHH', self.eh, 0x36)
        if self.e_phentsize != 0x38 or (self.e_shnum and self.e_shentsize != 0x40):
            raise Ps3ToolError('unexpected ELF header/entry sizes')
        self._need(self.phdr_off, self.e_phnum * 0x38, 'program headers')
        self.phdrs = [struct.unpack_from('>IIQQQQQQ', data, self.phdr_off + i * 0x38)
                      for i in range(self.e_phnum)]

    def _need(self, off, n, what):
        if off + n > len(self.d):
            raise Ps3ToolError('the SELF is truncated (%s)' % what)

    @property
    def type_name(self):
        return PROGRAM_TYPES.get(self.program_type, str(self.program_type))

    def npd(self):
        ctrl = self.d[self.ctrl_off:self.ctrl_off + self.ctrl_size]
        i = ctrl.find(b'NPD\0')
        if i < 0:
            return None
        lic = struct.unpack_from('>I', ctrl, i + 8)[0]
        cid = ctrl[i + 0x10:i + 0x40].split(b'\0', 1)[0].decode('ascii', 'replace')
        return lic, cid

    def klicensee(self, keys):
        n = self.npd()
        if n is None:
            return None
        lic, cid = n
        if lic == 3:
            return keys.get('np_klic_free', 16)
        if lic in (1, 2):
            rap = find_rap(self.path, cid)
            if rap is None:
                raise Ps3ToolError('this is a PSN (NPDRM) SELF; its .rap was not found. Put %s.rap '
                                   'next to it or in RPCS3\'s exdata folder' % cid)
            return rap_to_rif(rap, keys.get('rap_key', 16))
        raise Ps3ToolError('NPDRM licence type %d is not known' % lic)

    def metadata(self, erk, riv, klic, keys):
        off = 0x20 + self.meta_off
        self._need(off, 0x40, 'metadata info')
        info = self.d[off:off + 0x40]
        if not self.key_revision & KEY_REV_DEBUG:
            if klic is not None:
                k = ecb_decrypt(keys.get('np_klic_key', 16), klic)
                info = cbc_decrypt(k, bytes(16), info)
            info = cbc_decrypt(erk, riv, info)
        return sce_metadata(self.d, off, self.header_len, info)

    def to_elf(self, keys):
        if self.key_revision & KEY_REV_DEBUG:
            erk = riv = None
        else:
            erk, riv = keys.self_key(self.type_name, self.key_revision)
        klic = self.klicensee(keys)
        sections, dkeys = self.metadata(erk, riv, klic, keys)

        out = bytearray()

        def put(o, blob):
            if o + len(blob) > len(out):
                out.extend(bytes(o + len(blob) - len(out)))
            out[o:o + len(blob)] = blob

        put(0, self.eh)
        put(0x40, self.d[self.phdr_off:self.phdr_off + self.e_phnum * 0x38])
        for s in sections:
            if s['type'] != 2:
                continue
            if s['prog_idx'] >= len(self.phdrs):
                raise Ps3ToolError('section names program header %d of %d'
                                   % (s['prog_idx'], len(self.phdrs)))
            ph = self.phdrs[s['prog_idx']]
            p_offset, p_filesz = ph[2], ph[5]
            self._need(s['data_off'], s['data_size'], 'segment data')
            blob = self.d[s['data_off']:s['data_off'] + s['data_size']]
            if s['encrypted'] == 3:
                if s['key_idx'] >= len(dkeys) or s['iv_idx'] >= len(dkeys):
                    raise Ps3ToolError('section key index out of range')
                blob = ctr_crypt(dkeys[s['key_idx']], dkeys[s['iv_idx']], blob)
            if s['compressed'] == 2:
                try:
                    blob = zlib.decompressobj().decompress(blob, p_filesz)
                except zlib.error as e:
                    raise Ps3ToolError('segment %d does not inflate (%s): wrong key?'
                                       % (s['prog_idx'], e))
            put(p_offset, blob)
        if self.shdr_off and self.e_shnum:
            self._need(self.shdr_off, self.e_shnum * 0x40, 'section headers')
            put(self.e_shoff, self.d[self.shdr_off:self.shdr_off + self.e_shnum * 0x40])
        return bytes(out)


def check_elf(elf, prx):
    if elf[:4] != b'\x7fELF':
        return 'no ELF magic'
    e_type = struct.unpack_from('>H', elf, 0x10)[0]
    e_entry, e_phoff = struct.unpack_from('>QQ', elf, 0x18)
    phnum = struct.unpack_from('>H', elf, 0x38)[0]
    loads = []
    for i in range(phnum):
        p = struct.unpack_from('>IIQQQQQQ', elf, e_phoff + i * 0x38)
        if p[0] == 1:
            loads.append(p)
    if not loads:
        return 'no PT_LOAD segment'
    if prx or e_type == 0xFFA4:
        return None
    end = max(p[3] + p[5] for p in loads)
    for p in loads:
        if p[3] <= e_entry < p[3] + p[5]:
            fo = p[2] + (e_entry - p[3])
            func, toc = struct.unpack_from('>II', elf, fo)
            if 0 < func < end and 0 < toc < end:
                return None
            return 'entry descriptor (func 0x%X, toc 0x%X) points outside the image' % (func, toc)
    return 'entry point 0x%X is outside every PT_LOAD segment' % e_entry


def _write_file(path, data):
    try:
        with open(path, 'wb') as f:
            f.write(data)
    except OSError as e:
        try:
            os.remove(path)
        except OSError:
            pass
        raise Ps3ToolError('cannot write %s: %s' % (path, e))


def decrypt_self(in_path, out_path, keys):
    if os.path.abspath(in_path) == os.path.abspath(out_path):
        raise Ps3ToolError('the input and output are the same file')
    try:
        with open(in_path, 'rb') as f:
            data = f.read()
    except OSError as e:
        raise Ps3ToolError('cannot read %s: %s' % (in_path, e))
    if data[:4] == b'\x7fELF':
        _write_file(out_path, data)
        return
    s = Self(data, in_path)
    elf = s.to_elf(keys)
    bad = check_elf(elf, prx=in_path.lower().endswith(('.sprx', '.prx')))
    if bad:
        raise Ps3ToolError('the decrypted ELF does not look right (%s): wrong key?' % bad)
    _write_file(out_path, elf)


# ---------------------------------------------------------------------------
# Game update PKG -> folder
# ---------------------------------------------------------------------------
def _pkg_keystream(used, riv, hdr60, start_block, nblocks, keys):
    if used in ('finalized', 'psp'):
        base = int.from_bytes(riv, 'big')
        ctrs = b''.join(((base + start_block + i) & ((1 << 128) - 1)).to_bytes(16, 'big')
                        for i in range(nblocks))
        key = keys.get('ps3_gpkg_key', 16) if used == 'finalized' else keys.get('psp_pkg_key', 16)
        return ecb_encrypt(key, ctrs)
    qa = hdr60[0:0x10]
    out = bytearray()
    for i in range(nblocks):
        k = bytearray(0x40)
        k[0x00:0x08] = qa[0:8]
        k[0x08:0x10] = qa[0:8]
        k[0x10:0x18] = qa[8:16]
        k[0x18:0x20] = qa[8:16]
        k[0x38:0x40] = ((start_block + i) & ((1 << 64) - 1)).to_bytes(8, 'big')
        out += hashlib.sha1(bytes(k)).digest()[:0x10]
    return bytes(out)


def _pkg_out_path(out_dir, name):
    parts = []
    for p in name.replace('\\', '/').split('/'):
        p = p.strip()
        if not p or p in ('.', '..') or os.path.splitdrive(p)[0]:
            continue
        parts.append(p)
    if not parts:
        raise Ps3ToolError('package entry has no usable name: %r' % name)
    return os.path.join(out_dir, *parts)


def extract_pkg(in_path, out_dir, keys):
    try:
        f = open(in_path, 'rb')
    except OSError as e:
        raise Ps3ToolError('cannot read %s: %s' % (in_path, e))
    with f:
        hdr = f.read(0xA0)
        if len(hdr) < 0xA0:
            raise Ps3ToolError('not a PS3 game update PKG (file too small): %s' % in_path)
        if hdr[:4] == b'SCE\0':
            raise Ps3ToolError('this is a firmware package, not a game update PKG; use the pup mode')
        if hdr[:4] != b'\x7fPKG':
            raise Ps3ToolError('not a PS3 game update PKG (no \\x7fPKG magic): %s' % in_path)

        rev = struct.unpack('>H', hdr[4:6])[0]
        item_count = struct.unpack('>I', hdr[0x14:0x18])[0]
        data_off = struct.unpack('>Q', hdr[0x20:0x28])[0]
        data_size = struct.unpack('>Q', hdr[0x28:0x30])[0]
        riv = hdr[0x70:0x80]
        hdr60 = hdr[0x60:0xA0]

        def raw(off, n):
            f.seek(data_off + off)
            return f.read(n)

        def decrypt_range(used, start, length):
            sb = start // 16
            off = start - sb * 16
            nb = (off + length + 15) // 16
            chunk = raw(sb * 16, nb * 16)
            ks = _pkg_keystream(used, riv, hdr60, sb, min(nb, len(chunk) // 16 + 1), keys)
            m = min(len(chunk), len(ks))
            a = int.from_bytes(chunk[:m], 'big') ^ int.from_bytes(ks[:m], 'big')
            dec = a.to_bytes(m, 'big')
            return dec[off:off + length]

        def parse_table(used):
            hdr_len = min(data_size, item_count * 0x20 + (1 << 20))
            dec = decrypt_range(used, 0, hdr_len)
            alt = {used: dec}

            def with_key(k, off, n):
                if k not in alt:
                    alt[k] = decrypt_range(k, 0, hdr_len)
                return alt[k][off:off + n]

            names = []
            for i in range(item_count):
                e = dec[i * 0x20:(i + 1) * 0x20]
                if len(e) < 0x20:
                    return None
                no, ns = struct.unpack('>II', e[:8])
                fo, fs = struct.unpack('>QQ', e[8:0x18])
                flags = struct.unpack('>I', e[0x18:0x1C])[0]
                k = used if used == 'debug' else ('psp' if flags & 0x10000000 else 'finalized')
                nm = with_key(k, no, ns)
                if not nm or any(c < 0x20 or c > 0x7E for c in nm):
                    return None
                names.append((nm.decode(), fo, fs, k))
            return names

        used = None
        for cand in (('debug' if rev == 0 else 'finalized'), 'finalized', 'psp', 'debug'):
            names = parse_table(cand)
            if names:
                used = cand
                break
        if not used:
            raise Ps3ToolError('could not decrypt the PKG\'s file table (not a PS3 PKG, or damaged)')

        try:
            os.makedirs(out_dir, exist_ok=True)
        except OSError as e:
            raise Ps3ToolError('cannot create %s: %s' % (out_dir, e))

        count = 0
        for nm, fo, fs, k in names:
            if fs <= 0:
                continue
            blob = decrypt_range(k, fo, fs)
            op = _pkg_out_path(out_dir, nm)
            try:
                os.makedirs(os.path.dirname(op), exist_ok=True)
                with open(op, 'wb') as g:
                    g.write(blob)
            except OSError as e:
                raise Ps3ToolError('cannot write %s: %s' % (op, e))
            count += 1
        return count


# ---------------------------------------------------------------------------
# PS3UPDAT.PUP -> dev_flash folder
# ---------------------------------------------------------------------------
PUP_MAGIC = b'SCEUF\0\0\0'
PUP_ENTRIES = {0x100: 'version.txt', 0x101: 'license.xml', 0x102: 'promo_flags.txt',
               0x103: 'update_flags.txt', 0x104: 'patch_build.txt', 0x200: 'ps3swu.self',
               0x201: 'vsh.tar', 0x202: 'dots.txt', 0x203: 'patch_data.pkg',
               0x300: 'update_files.tar', 0x501: 'spkg_hdr.tar', 0x601: 'ps3swu2.self'}
ENTRY_UPDATE_FILES = 0x300
SCE_TYPE_PKG = 3
DEV_FLASH_PKG = re.compile(r'^dev_flash_\d+\.tar\.aa\.')


class Window:
    """A read-only, seekable view of [start, start+size) of an open file (for tarfile)."""

    def __init__(self, f, start, size):
        self.f, self.start, self.size, self.pos = f, start, size, 0

    def tell(self):
        return self.pos

    def seek(self, off, whence=0):
        base = (0, self.pos, self.size)[whence]
        self.pos = max(0, base + off)
        return self.pos

    def read(self, n=-1):
        left = self.size - self.pos
        n = left if n is None or n < 0 else min(n, left)
        if n <= 0:
            return b''
        self.f.seek(self.start + self.pos)
        b = self.f.read(n)
        self.pos += len(b)
        return b


class Pup:
    def __init__(self, path):
        self.path = path
        try:
            self.f = open(path, 'rb')
        except OSError as e:
            raise Ps3ToolError('cannot read %s: %s' % (path, e))
        try:
            self._parse()
        except Exception:
            self.f.close()
            raise

    def close(self):
        self.f.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _parse(self):
        size = os.fstat(self.f.fileno()).st_size
        h = self.f.read(0x30)
        if len(h) < 0x30 or h[:8] != PUP_MAGIC:
            raise Ps3ToolError('not a PS3 system update (no SCEUF magic): %s' % self.path)
        (_, self.package_version, self.image_version, n,
         self.header_length, self.data_length) = struct.unpack('>8sQQQQQ', h)
        if not 0 < n < 256 or 0x30 + n * 0x40 > size:
            raise Ps3ToolError('%s: implausible file table (%d entries)' % (self.path, n))
        tbl = self.f.read(n * 0x20)
        self.entries = {}
        for i in range(n):
            eid, off, ln = struct.unpack_from('>QQQ', tbl, i * 0x20)
            if off + ln > size:
                raise Ps3ToolError('%s is truncated (entry 0x%X ends at 0x%X, the file at 0x%X)'
                                   % (self.path, eid, off + ln, size))
            self.entries[eid] = (off, ln)
        self._tar = None
        self._members = None

    def entry(self, eid):
        if eid not in self.entries:
            raise Ps3ToolError('%s has no %s (entry 0x%X)'
                               % (self.path, PUP_ENTRIES.get(eid, '?'), eid))
        off, ln = self.entries[eid]
        return Window(self.f, off, ln)

    def update_files(self):
        if self._tar is None:
            try:
                self._tar = tarfile.open(fileobj=self.entry(ENTRY_UPDATE_FILES), mode='r:')
                self._members = self._tar.getmembers()
            except tarfile.TarError as e:
                raise Ps3ToolError('update_files.tar does not read (%s)' % e)
        return self._tar

    def packages(self):
        self.update_files()
        return sorted((m for m in self._members if m.isfile() and DEV_FLASH_PKG.match(m.name)),
                      key=lambda m: m.name)

    def read(self, member, n=None):
        f = self.update_files().extractfile(member)
        return f.read() if n is None else f.read(n)


def _sce_header(d, what):
    if len(d) < 0x20 or d[:4] != b'SCE\0':
        raise Ps3ToolError('%s: not an SCE container' % what)
    _, version, key_rev, htype, meta_off, header_len, data_len = struct.unpack_from('>4sIHHIQQ', d, 0)
    if htype != SCE_TYPE_PKG:
        raise Ps3ToolError('%s: SCE header type %d is not a PKG' % (what, htype))
    return version, key_rev, meta_off, header_len, data_len


def _pup_decrypt_pkg(d, what, keys):
    _v, key_rev, meta_off, header_len, _dl = _sce_header(d, what)
    off = 0x20 + meta_off
    if off + 0x40 > len(d):
        raise Ps3ToolError('%s is truncated (metadata info)' % what)
    raw = d[off:off + 0x40]
    if key_rev & KEY_REV_DEBUG:
        info = raw
    else:
        info = cbc_decrypt(keys.get('scepkg_erk', 32), keys.get('scepkg_riv', 16), raw)
    sections, dkeys = sce_metadata(d, off, header_len, info)
    out = []
    for i, s in enumerate(sections):
        if s['data_off'] + s['data_size'] > len(d):
            raise Ps3ToolError('%s is truncated (section %d)' % (what, i))
        blob = d[s['data_off']:s['data_off'] + s['data_size']]
        if s['encrypted'] == 3:
            if max(s['key_idx'], s['iv_idx']) >= len(dkeys):
                raise Ps3ToolError('%s: section key index out of range' % what)
            blob = ctr_crypt(dkeys[s['key_idx']], dkeys[s['iv_idx']], blob)
        if s['compressed'] == 2:
            try:
                z = zlib.decompressobj()
                blob = z.decompress(blob) + z.flush()
            except zlib.error as e:
                raise Ps3ToolError('%s: section %d does not inflate (%s)' % (what, i, e))
        out.append(blob)
    return out


def _pkg_tar(sections, what):
    for s in sections:
        if len(s) >= 512 and s[257:262] == b'ustar':
            return s
    if len(sections) > 2:
        return sections[2]
    raise Ps3ToolError('%s: no tar section' % what)


def _safe_path(name):
    parts = name.replace('\\', '/').split('/')
    if '..' in parts:
        return None
    p = posixpath.normpath('/' + '/'.join(parts)).lstrip('/')
    if p == 'dev_flash' or p.startswith('dev_flash/'):
        p = p[len('dev_flash/'):]
    return p or None


def extract_pup(in_path, out_dir, keys):
    with Pup(in_path) as pup:
        pkgs = pup.packages()
        if not pkgs:
            raise Ps3ToolError('%s: no dev_flash packages in update_files.tar' % in_path)
        try:
            os.makedirs(out_dir, exist_ok=True)
        except OSError as e:
            raise Ps3ToolError('cannot create %s: %s' % (out_dir, e))
        count = 0
        for m in pkgs:
            sections = _pup_decrypt_pkg(pup.read(m), m.name, keys)
            tar = _pkg_tar(sections, m.name)
            try:
                with tarfile.open(fileobj=io.BytesIO(tar), mode='r:') as t:
                    for ti in t.getmembers():
                        p = _safe_path(ti.name)
                        if p is None:
                            continue
                        dst = os.path.join(out_dir, *p.split('/'))
                        if ti.isdir():
                            os.makedirs(dst, exist_ok=True)
                        elif ti.isfile():
                            try:
                                os.makedirs(os.path.dirname(dst), exist_ok=True)
                                with open(dst, 'wb') as g:
                                    g.write(t.extractfile(ti).read())
                            except OSError as e:
                                raise Ps3ToolError('cannot write %s: %s' % (dst, e))
                            count += 1
            except tarfile.TarError as e:
                raise Ps3ToolError('%s: the decrypted tar does not read (%s): wrong key?'
                                   % (m.name, e))
        return count


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(
        prog='ps3tool',
        description='Decrypt PS3 SELF files, extract game update PKGs and PS3UPDAT.PUP firmware.')
    sub = ap.add_subparsers(dest='mode', required=True)
    for mode, text in (('decrypt', 'EBOOT.BIN / *.sprx (SELF) -> decrypted ELF/PRX'),
                       ('pkg', 'game update .pkg -> folder (PARAM.SFO, USRDIR/...)'),
                       ('pup', 'PS3UPDAT.PUP -> dev_flash folder (sys/external/...)')):
        p = sub.add_parser(mode, help=text)
        p.add_argument('input', help='input file')
        p.add_argument('output', help='output file or folder')
        p.add_argument('--keys', help='key file (default: $PS3TOOL_KEYS or the standard locations)')
    a = ap.parse_args(argv)

    try:
        keys = Keys.load(a.keys)
        if a.mode == 'decrypt':
            decrypt_self(a.input, a.output, keys)
        elif a.mode == 'pkg':
            extract_pkg(a.input, a.output, keys)
        elif a.mode == 'pup':
            extract_pup(a.input, a.output, keys)
    except Ps3ToolError as e:
        print('ps3tool: %s' % e, file=sys.stderr)
        return 1
    except OSError as e:
        print('ps3tool: %s' % e, file=sys.stderr)
        return 1
    except Exception as e:
        if os.environ.get('PS3TOOL_DEBUG'):
            raise
        print('ps3tool: %s' % e, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
