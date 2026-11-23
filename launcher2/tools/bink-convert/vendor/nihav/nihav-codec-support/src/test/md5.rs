use std::fmt;

const MD5_SHIFTS: [u8; 64] = [
    7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,
    5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,
    4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,
    6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21
];

const MD5_K: [u32; 64] = [
    0xD76AA478, 0xE8C7B756, 0x242070DB, 0xC1BDCEEE, 0xF57C0FAF, 0x4787C62A, 0xA8304613, 0xFD469501,
    0x698098D8, 0x8B44F7AF, 0xFFFF5BB1, 0x895CD7BE, 0x6B901122, 0xFD987193, 0xA679438E, 0x49B40821,
    0xF61E2562, 0xC040B340, 0x265E5A51, 0xE9B6C7AA, 0xD62F105D, 0x02441453, 0xD8A1E681, 0xE7D3FBC8,
    0x21E1CDE6, 0xC33707D6, 0xF4D50D87, 0x455A14ED, 0xA9E3E905, 0xFCEFA3F8, 0x676F02D9, 0x8D2A4C8A,
    0xFFFA3942, 0x8771F681, 0x6D9D6122, 0xFDE5380C, 0xA4BEEA44, 0x4BDECFA9, 0xF6BB4B60, 0xBEBFBC70,
    0x289B7EC6, 0xEAA127FA, 0xD4EF3085, 0x04881D05, 0xD9D4D039, 0xE6DB99E5, 0x1FA27CF8, 0xC4AC5665,
    0xF4292244, 0x432AFF97, 0xAB9423A7, 0xFC93A039, 0x655B59C3, 0x8F0CCC92, 0xFFEFF47D, 0x85845DD1,
    0x6FA87E4F, 0xFE2CE6E0, 0xA3014314, 0x4E0811A1, 0xF7537E82, 0xBD3AF235, 0x2AD7D2BB, 0xEB86D391
];

const INITIAL_MD5_HASH: [u32; 4] = [ 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476 ];

fn box0(b: u32, c: u32, d: u32) -> u32 { (b & c) | (!b & d) }
fn box1(b: u32, c: u32, d: u32) -> u32 { (d & b) | (!d & c) }
fn box2(b: u32, c: u32, d: u32) -> u32 { b ^ c ^ d }
fn box3(b: u32, c: u32, d: u32) -> u32 { c ^ (b | !d) }

#[derive(Clone)]
pub struct MD5 {
    pub hash:   [u32; 4],
    inwords:    [u32; 16],
    buf:        [u8; 64],
    pos:        usize,
    count:      usize,
}

impl PartialEq for MD5 {
    fn eq(&self, other: &Self) -> bool {
        self.hash == other.hash
    }
}

impl Eq for MD5 { }

impl fmt::Display for MD5 {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "{:08x}{:08x}{:08x}{:08x}", self.hash[0].swap_bytes(), self.hash[1].swap_bytes(), self.hash[2].swap_bytes(), self.hash[3].swap_bytes())
    }
}

macro_rules! round {
    ($a: ident, $b: ident, $c: ident, $d: ident, $box: ident, $k: expr, $inval: expr) => {
        let f = $box($b, $c, $d).wrapping_add($a).wrapping_add(MD5_K[$k]).wrapping_add($inval);
        $a = $d;
        $d = $c;
        $c = $b;
        $b = $b.wrapping_add(f.rotate_left(MD5_SHIFTS[$k].into()));
    }
}

#[allow(dead_code)]
impl MD5 {
    pub fn new() -> Self {
        Self {
            hash:       INITIAL_MD5_HASH,
            inwords:    [0; 16],
            buf:        [0; 64],
            pos:        0,
            count:      0,
        }
    }
    fn calc_one_block(&mut self) {
        let mut a = self.hash[0];
        let mut b = self.hash[1];
        let mut c = self.hash[2];
        let mut d = self.hash[3];

        for (out, src) in self.inwords.iter_mut().zip(self.buf.chunks_exact(4)) {
            *out = (u32::from(src[0]) <<  0) |
                   (u32::from(src[1]) <<  8) |
                   (u32::from(src[2]) << 16) |
                   (u32::from(src[3]) << 24);
        }

        for k in  0..16 { round!(a, b, c, d, box0, k, self.inwords[k]); }
        for k in 16..32 { round!(a, b, c, d, box1, k, self.inwords[(5 * k + 1) & 0xF]); }
        for k in 32..48 { round!(a, b, c, d, box2, k, self.inwords[(3 * k + 5) & 0xF]); }
        for k in 48..64 { round!(a, b, c, d, box3, k, self.inwords[(7 * k)     & 0xF]); }

        self.hash[0] = self.hash[0].wrapping_add(a);
        self.hash[1] = self.hash[1].wrapping_add(b);
        self.hash[2] = self.hash[2].wrapping_add(c);
        self.hash[3] = self.hash[3].wrapping_add(d);

        self.pos = 0;
    }
    pub fn update_hash(&mut self, src: &[u8]) {
        for byte in src.iter() {
            self.buf[self.pos] = *byte;
            self.pos += 1;
            if self.pos == 64 {
                self.calc_one_block();
            }
        }
        self.count += src.len();
    }
    pub fn finish(&mut self) {
        self.buf[self.pos] = 0x80;
        self.pos += 1;
        if self.pos > 48 {
            while self.pos < 64 {
                self.buf[self.pos] = 0x00;
                self.pos += 1;
            }
            self.calc_one_block();
        }
        while self.pos < 64 {
            self.buf[self.pos] = 0x00;
            self.pos += 1;
        }
        for i in 0..8 {
            self.buf[56 + i] = ((self.count * 8) >> (i * 8)) as u8;
        }
        self.calc_one_block();
    }
    pub fn get_hash(&self, dst: &mut [u32; 4]) {
        for (dst, src) in dst.iter_mut().zip(self.hash.iter()) {
            *dst = src.swap_bytes();
        }
    }
    pub fn get_hash_bytes(&self, dst: &mut [u8; 4]) {
        for (dst, src) in dst.chunks_exact_mut(4).zip(self.hash.iter()) {
            dst[0] = (*src >>  0) as u8;
            dst[1] = (*src >>  8) as u8;
            dst[2] = (*src >> 16) as u8;
            dst[3] = (*src >> 24) as u8;
        }
    }
    pub fn calculate_hash(src: &[u8], hash: &mut [u32; 4]) {
        let mut md5 = Self::new();
        md5.update_hash(src);
        md5.finish();
        md5.get_hash(hash);
    }
}

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn test_md5() {
        let mut hash = [0u32; 4];

        MD5::calculate_hash(&[], &mut hash);
        assert_eq!(hash, [ 0xD41D8CD9, 0x8F00B204, 0xE9800998, 0xECF8427E ]);

        MD5::calculate_hash(b"abc", &mut hash);
        assert_eq!(hash, [ 0x90015098, 0x3CD24FB0, 0xD6963F7D, 0x28E17F72 ]);

        MD5::calculate_hash(b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", &mut hash);
        assert_eq!(hash, [ 0x8215EF07, 0x96A20BCA, 0xAAE116D3, 0x876C664A ]);

        let mut md5 = MD5::new();
        for _ in 0..1000000 {
            md5.update_hash(b"a");
        }
        md5.finish();
        md5.get_hash(&mut hash);
        assert_eq!(hash, [ 0x7707D6AE, 0x4E027C70, 0xEEA2A935, 0xC2296F21 ]);
    }
}
