use super::*;
use super::kernel::Kernel;

const DEFAULT_YUV: usize = 4;

const YUV_PARAMS: &[[f32; 2]] = &[
    [ 0.333,    0.333   ], // RGB
    [ 0.2126,   0.0722  ], // ITU-R BT709
    [ 0.333,    0.333   ], // unspecified
    [ 0.333,    0.333   ], // reserved
    [ 0.299,    0.114   ], // ITU-R BT601
    [ 0.299,    0.114   ], // ITU-R BT470
    [ 0.299,    0.114   ], // SMPTE 170M
    [ 0.212,    0.087   ], // SMPTE 240M
    [ 0.333,    0.333   ], // YCoCg
    [ 0.2627,   0.0593  ], // ITU-R BT2020
    [ 0.2627,   0.0593  ], // ITU-R BT2020
];

fn parse_yuv_mat(name: &str) -> usize {
    match name {
        "rgb"       => 0,
        "bt709"     => 1,
        "bt601"     => 4,
        "bt470"     => 5,
        "smpte170m" => 6,
        "smpte240m" => 7,
        "ycocg"     => 8,
        "bt2020"    => 9,
        _ => 2,
    }
}

/*fn get_yuv_mat(id: usize) -> &'static str {
    match id {
        1 => "bt709",
        4 => "bt601",
        5 => "bt470",
        6 => "smpte170m",
        7 => "smpte240m",
        8 => "ycocg",
        9 => "bt2020",
        _ => "rgb",
    }
}*/

const BT_PAL_COEFFS: [f32; 2] = [ 0.493, 0.877 ];

const SMPTE_NTSC_COEFFS: &[f32; 4] = &[ -0.268, 0.7358, 0.4127, 0.4778 ];

/*const RGB2YCOCG: [[f32; 3]; 3] = [
    [  0.25,  0.5,  0.25 ],
    [ -0.25,  0.5, -0.25 ],
    [  0.5,   0.0, -0.5  ]
];
const YCOCG2RGB: [[f32; 3]; 3] = [
    [ 1.0, -1.0,  1.0 ],
    [ 1.0,  1.0,  0.0 ],
    [ 1.0, -1.0, -1.0 ]
];

const XYZ2RGB: [[f32; 3]; 3] = [
    [ 0.49,    0.31,   0.2     ],
    [ 0.17697, 0.8124, 0.01063 ],
    [ 0.0,     0.01,   0.99    ]
];
const RGB2XYZ: [[f32; 3]; 3] = [
    [  2.364613, -0.89654, -0.46807 ],
    [ -0.515167,  1.42641,  0.08876 ],
    [  0.0052,   -0.01441,  1.00920 ]
];*/

fn make_rgb2yuv(kr: f32, kb: f32, mat: &mut [[f32; 3]; 3]) {
    // Y
    mat[0][0] = kr;
    mat[0][1] = 1.0 - kr - kb;
    mat[0][2] = kb;
    // Cb
    mat[1][0] = -mat[0][0] * 0.5 / (1.0 - kb);
    mat[1][1] = -mat[0][1] * 0.5 / (1.0 - kb);
    mat[1][2] = 0.5;
    // Cr
    mat[2][0] = 0.5;
    mat[2][1] = -mat[0][1] * 0.5 / (1.0 - kr);
    mat[2][2] = -mat[0][2] * 0.5 / (1.0 - kr);
}

fn make_yuv2rgb(kr: f32, kb: f32, mat: &mut [[f32; 3]; 3]) {
    let kg = 1.0 - kr - kb;

    // R
    mat[0][0] = 1.0;
    mat[0][1] = 0.0;
    mat[0][2] = 2.0 * (1.0 - kr);
    // G
    mat[1][0] = 1.0;
    mat[1][1] = -kb * 2.0 * (1.0 - kb) / kg;
    mat[1][2] = -kr * 2.0 * (1.0 - kr) / kg;
    // B
    mat[2][0] = 1.0;
    mat[2][1] = 2.0 * (1.0 - kb);
    mat[2][2] = 0.0;
}

fn apply_pal_rgb2yuv(eu: f32, ev: f32, mat: &mut [[f32; 3]; 3]) {
    let ufac = 2.0 * (1.0 - mat[0][2]) * eu;
    let vfac = 2.0 * (1.0 - mat[0][0]) * ev;

    // U
    mat[1][0] *= ufac;
    mat[1][1] *= ufac;
    mat[1][2]  = eu * (1.0 - mat[0][2]);
    // V
    mat[2][0]  = ev * (1.0 - mat[0][0]);
    mat[2][1] *= vfac;
    mat[2][2] *= vfac;
}

fn apply_pal_yuv2rgb(eu: f32, ev: f32, mat: &mut [[f32; 3]; 3]) {
    let ufac = 1.0 / (mat[2][1] * eu);
    let vfac = 1.0 / (mat[0][2] * ev);

    // R
    mat[0][2] *= vfac;
    // G
    mat[1][1] *= ufac;
    mat[1][2] *= vfac;
    // B
    mat[2][1] *= ufac;
}

fn apply_ntsc_rgb2yiq(params: &[f32; 4], mat: &mut [[f32; 3]; 3]) {
    let ufac = 2.0 * (1.0 - mat[0][2]);
    let vfac = 2.0 * (1.0 - mat[0][0]);
    let mut tmp: [[f32; 3]; 2] = [[0.0; 3]; 2];

    for i in 0..3 {
        tmp[0][i] = mat[1][i] * ufac;
        tmp[1][i] = mat[2][i] * vfac;
    }
    for i in 0..3 {
        mat[1][i] = params[0] * tmp[0][i] + params[1] * tmp[1][i];
        mat[2][i] = params[2] * tmp[0][i] + params[3] * tmp[1][i];
    }
}

fn subm_det(mat: &[[f32; 3]; 3], col: usize, row: usize) -> f32 {
    let row0 = if row == 0 { 1 } else { 0 };
    let row1 = if (row == 1) || (row0 == 1) { 2 } else { 1 };
    let col0 = if col == 0 { 1 } else { 0 };
    let col1 = if (col == 1) || (col0 == 1) { 2 } else { 1 };

    let det = mat[row0][col0] * mat[row1][col1] - mat[row0][col1] * mat[row1][col0];
    if ((col ^ row) & 1) == 0 {
        det
    } else {
        -det
    }
}

fn invert_matrix(mat: &mut [[f32; 3]; 3]) {
    let d00 = subm_det(mat, 0, 0);
    let d01 = subm_det(mat, 0, 1);
    let d02 = subm_det(mat, 0, 2);
    let d10 = subm_det(mat, 1, 0);
    let d11 = subm_det(mat, 1, 1);
    let d12 = subm_det(mat, 1, 2);
    let d20 = subm_det(mat, 2, 0);
    let d21 = subm_det(mat, 2, 1);
    let d22 = subm_det(mat, 2, 2);
    let det = 1.0 / (mat[0][0] * d00 + mat[0][1] * d10 + mat[0][2] * d20).abs();

    mat[0][0] = det * d00;
    mat[0][1] = det * d01;
    mat[0][2] = det * d02;
    mat[1][0] = det * d10;
    mat[1][1] = det * d11;
    mat[1][2] = det * d12;
    mat[2][0] = det * d20;
    mat[2][1] = det * d21;
    mat[2][2] = det * d22;
}

fn matrix_mul(mat: &[[f32; 3]; 3], a: f32, b: f32, c: f32) -> (f32, f32, f32) {
    (a * mat[0][0] + b * mat[0][1] + c * mat[0][2],
     a * mat[1][0] + b * mat[1][1] + c * mat[1][2],
     a * mat[2][0] + b * mat[2][1] + c * mat[2][2] )
}

#[derive(Default)]
struct RgbToYuv {
    matrix: [[f32; 3]; 3],
    mode:   usize,
}

impl RgbToYuv {
    fn new() -> Self { Self::default() }
}

#[allow(clippy::many_single_char_names)]
impl Kernel for RgbToYuv {
    fn init(&mut self, in_fmt: &ScaleInfo, dest_fmt: &ScaleInfo, options: &[(String, String)]) -> ScaleResult<NABufferType> {
        let mut debug = false;
        let mut mode = DEFAULT_YUV;
        for (name, value) in options.iter() {
            match (name.as_str(), value.as_str()) {
                ("debug", "")     => { debug = true; },
                ("debug", "true") => { debug = true; },
                ("rgb2yuv.mode", ymode) => {
                    mode = parse_yuv_mat(ymode);
                },
                _ => {},
            }
        }
        self.mode = mode;

        let mut df = dest_fmt.fmt;
        make_rgb2yuv(YUV_PARAMS[mode][0], YUV_PARAMS[mode][1], &mut self.matrix);
        if let ColourModel::YUV(yuvsm) = df.get_model() {
            match yuvsm {
            YUVSubmodel::YCbCr  => {},
            YUVSubmodel::YIQ    => { apply_ntsc_rgb2yiq(SMPTE_NTSC_COEFFS, &mut self.matrix); },
            YUVSubmodel::YUVJ   => { apply_pal_rgb2yuv(BT_PAL_COEFFS[0], BT_PAL_COEFFS[1], &mut self.matrix); },
            };
        } else {
            return Err(ScaleError::InvalidArgument);
        }
        for i in 0..MAX_CHROMATONS {
            if let Some(ref mut chr) = df.comp_info[i] {
                chr.packed = false;
                chr.comp_offs = i as u8;
                chr.h_ss = 0;
                chr.v_ss = 0;
            }
        }
        if debug {
            println!(" [intermediate format {}]", df);
        }
        let res = alloc_video_buffer(NAVideoInfo::new(in_fmt.width, in_fmt.height, false, df), 3);
        if res.is_err() { return Err(ScaleError::AllocError); }
        Ok(res.unwrap())
    }
    fn process(&mut self, pic_in: &NABufferType, pic_out: &mut NABufferType) {
        if let (Some(ref sbuf), Some(ref mut dbuf)) = (pic_in.get_vbuf(), pic_out.get_vbuf()) {
            if dbuf.get_info().get_format().get_num_comp() < 3 {
                return self.process_greyscale(sbuf, dbuf);
            }
            let istrides = [sbuf.get_stride(0), sbuf.get_stride(1), sbuf.get_stride(2)];
            let dstrides = [dbuf.get_stride(0), dbuf.get_stride(1), dbuf.get_stride(2)];
            let (w, h) = sbuf.get_dimensions(0);

            let mut roff = sbuf.get_offset(0);
            let mut goff = sbuf.get_offset(1);
            let mut boff = sbuf.get_offset(2);
            let mut yoff = dbuf.get_offset(0);
            let mut uoff = dbuf.get_offset(1);
            let mut voff = dbuf.get_offset(2);
            let src = sbuf.get_data();
            let dst = dbuf.get_data_mut().unwrap();
            for _y in 0..h {
                for x in 0..w {
                    let r = f32::from(src[roff + x]);
                    let g = f32::from(src[goff + x]);
                    let b = f32::from(src[boff + x]);
                    let (y, u, v) = matrix_mul(&self.matrix, r, g, b);

                    dst[yoff + x] = (y as i16).clamp(0, 255) as u8;
                    dst[uoff + x] = ((u as i16).clamp(-128, 127) + 128) as u8;
                    dst[voff + x] = ((v as i16).clamp(-128, 127) + 128) as u8;
                }
                roff += istrides[0];
                goff += istrides[1];
                boff += istrides[2];
                yoff += dstrides[0];
                uoff += dstrides[1];
                voff += dstrides[2];
            }
        } else if let (Some(ref sbuf), Some(ref mut dbuf)) = (pic_in.get_vbuf16(), pic_out.get_vbuf16()) {
            if dbuf.get_info().get_format().get_num_comp() < 3 {
                return self.process_greyscale16(sbuf, dbuf);
            }
            let istrides = [sbuf.get_stride(0), sbuf.get_stride(1), sbuf.get_stride(2)];
            let dstrides = [dbuf.get_stride(0), dbuf.get_stride(1), dbuf.get_stride(2)];
            let (w, h) = sbuf.get_dimensions(0);
            let ydepth = dbuf.get_info().get_format().get_chromaton(0).unwrap().get_depth();
            let udepth = dbuf.get_info().get_format().get_chromaton(1).unwrap().get_depth();
            let vdepth = dbuf.get_info().get_format().get_chromaton(2).unwrap().get_depth();
            let ymax = (1 << ydepth) - 1;
            let umin = 1 << (udepth - 1);
            let vmin = 1 << (vdepth - 1);

            let mut roff = sbuf.get_offset(0);
            let mut goff = sbuf.get_offset(1);
            let mut boff = sbuf.get_offset(2);
            let mut yoff = dbuf.get_offset(0);
            let mut uoff = dbuf.get_offset(1);
            let mut voff = dbuf.get_offset(2);
            let src = sbuf.get_data();
            let dst = dbuf.get_data_mut().unwrap();
            for _y in 0..h {
                for x in 0..w {
                    let r = f32::from(src[roff + x]);
                    let g = f32::from(src[goff + x]);
                    let b = f32::from(src[boff + x]);
                    let (y, u, v) = matrix_mul(&self.matrix, r, g, b);

                    dst[yoff + x] = (y as i32).clamp(0, ymax) as u16;
                    dst[uoff + x] = ((u as i32).clamp(-umin, umin - 1) + umin) as u16;
                    dst[voff + x] = ((v as i32).clamp(-vmin, vmin - 1) + vmin) as u16;
                }
                roff += istrides[0];
                goff += istrides[1];
                boff += istrides[2];
                yoff += dstrides[0];
                uoff += dstrides[1];
                voff += dstrides[2];
            }
        } else {
            unimplemented!("rgb2yuv for unknown format");
        }
    }
}

impl RgbToYuv {
    fn process_greyscale(&self, sbuf: &NAVideoBuffer<u8>, dbuf: &mut NAVideoBuffer<u8>) {
        let istrides = [sbuf.get_stride(0), sbuf.get_stride(1), sbuf.get_stride(2)];
        let ystride = dbuf.get_stride(0);
        let (w, h) = sbuf.get_dimensions(0);

        let mut roff = sbuf.get_offset(0);
        let mut goff = sbuf.get_offset(1);
        let mut boff = sbuf.get_offset(2);
        let mut yoff = dbuf.get_offset(0);
        let src = sbuf.get_data();
        let dst = dbuf.get_data_mut().unwrap();
        for _y in 0..h {
            for x in 0..w {
                let r = f32::from(src[roff + x]);
                let g = f32::from(src[goff + x]);
                let b = f32::from(src[boff + x]);
                let (y, _u, _v) = matrix_mul(&self.matrix, r, g, b);

                dst[yoff + x] = (y as i16).clamp(0, 255) as u8;
            }
            roff += istrides[0];
            goff += istrides[1];
            boff += istrides[2];
            yoff += ystride;
        }
    }
    fn process_greyscale16(&self, sbuf: &NAVideoBuffer<u16>, dbuf: &mut NAVideoBuffer<u16>) {
        let istrides = [sbuf.get_stride(0), sbuf.get_stride(1), sbuf.get_stride(2)];
        let ystride = dbuf.get_stride(0);
        let (w, h) = sbuf.get_dimensions(0);
        let ydepth = dbuf.get_info().get_format().get_chromaton(0).unwrap().get_depth();

        let mut roff = sbuf.get_offset(0);
        let mut goff = sbuf.get_offset(1);
        let mut boff = sbuf.get_offset(2);
        let mut yoff = dbuf.get_offset(0);
        let src = sbuf.get_data();
        let dst = dbuf.get_data_mut().unwrap();
        for _y in 0..h {
            for x in 0..w {
                let r = f32::from(src[roff + x]);
                let g = f32::from(src[goff + x]);
                let b = f32::from(src[boff + x]);
                let (y, _u, _v) = matrix_mul(&self.matrix, r, g, b);

                dst[yoff + x] = (y as i32).clamp(0, (1 << ydepth) - 1) as u16;
            }
            roff += istrides[0];
            goff += istrides[1];
            boff += istrides[2];
            yoff += ystride;
        }
    }
}

pub fn create_rgb2yuv() -> Box<dyn Kernel> {
    Box::new(RgbToYuv::new())
}

#[derive(Default)]
struct YuvToRgb {
    matrix: [[f32; 3]; 3],
    mode:   usize,
    yscale: Vec<i16>,
    r_chr:  Vec<i16>,
    g_u:    Vec<i16>,
    g_v:    Vec<i16>,
    b_chr:  Vec<i16>,
}

impl YuvToRgb {
    fn new() -> Self { Self::default() }
}

#[allow(clippy::many_single_char_names)]
impl Kernel for YuvToRgb {
    fn init(&mut self, in_fmt: &ScaleInfo, dest_fmt: &ScaleInfo, options: &[(String, String)]) -> ScaleResult<NABufferType> {
        let mut debug = false;
        let mut mode = DEFAULT_YUV;
        for (name, value) in options.iter() {
            match (name.as_str(), value.as_str()) {
                ("debug", "")     => { debug = true; },
                ("debug", "true") => { debug = true; },
                ("yuv2rgb.mode", ymode) => {
                    mode = parse_yuv_mat(ymode);
                },
                _ => {},
            }
        }
        self.mode = mode;

        let mut df = dest_fmt.fmt;
        df.palette = false;
        if !df.is_unpacked() || df.get_max_depth() != 8 || df.get_total_depth() != df.get_num_comp() as u8 * 8 {
            df = NAPixelFormaton {
                    model: ColourModel::RGB(RGBSubmodel::RGB), components: 3,
                    comp_info: [
                        Some(NAPixelChromaton{ h_ss: 0, v_ss: 0, packed: false, depth: 8, shift: 0, comp_offs: 0, next_elem: 1 }),
                        Some(NAPixelChromaton{ h_ss: 0, v_ss: 0, packed: false, depth: 8, shift: 0, comp_offs: 1, next_elem: 1 }),
                        Some(NAPixelChromaton{ h_ss: 0, v_ss: 0, packed: false, depth: 8, shift: 0, comp_offs: 2, next_elem: 1 }),
                        None, None],
                    elem_size: 3, be: false, alpha: false, palette: false };
            if in_fmt.fmt.alpha && dest_fmt.fmt.alpha {
                df.alpha = true;
                df.components = 4;
                df.comp_info[3] = Some(NAPixelChromaton{ h_ss: 0, v_ss: 0, packed: false, depth: 8, shift: 0, comp_offs: 3, next_elem: 1 });
            }
        }
        make_yuv2rgb(YUV_PARAMS[mode][0], YUV_PARAMS[mode][1], &mut self.matrix);
        if let ColourModel::YUV(yuvsm) = in_fmt.fmt.get_model() {
            match yuvsm {
                YUVSubmodel::YCbCr  => {},
                YUVSubmodel::YIQ    => {
                    make_rgb2yuv(YUV_PARAMS[DEFAULT_YUV][0], YUV_PARAMS[DEFAULT_YUV][1], &mut self.matrix);
                    apply_ntsc_rgb2yiq(SMPTE_NTSC_COEFFS, &mut self.matrix);
                    invert_matrix(&mut self.matrix);
                },
                YUVSubmodel::YUVJ   => {
                    apply_pal_yuv2rgb(BT_PAL_COEFFS[0], BT_PAL_COEFFS[1], &mut self.matrix);
                },
            };
            if yuvsm != YUVSubmodel::YIQ {
                self.yscale = Vec::with_capacity(256);
                self.r_chr  = Vec::with_capacity(256);
                self.g_u    = Vec::with_capacity(256);
                self.g_v    = Vec::with_capacity(256);
                self.b_chr  = Vec::with_capacity(256);
                for i in 0..256 {
                    let yval = i as i16; // todo limited range as well
                    self.yscale.push(yval);
                    let rval = (((i as f32) - 128.0) * self.matrix[0][2]) as i16;
                    self.r_chr.push(rval);
                    let uval = (((i as f32) - 128.0) * self.matrix[1][1]) as i16;
                    self.g_u.push(uval);
                    let vval = (((i as f32) - 128.0) * self.matrix[1][2]) as i16;
                    self.g_v.push(vval);
                    let bval = (((i as f32) - 128.0) * self.matrix[2][1]) as i16;
                    self.b_chr.push(bval);
                }
            }
        } else {
            return Err(ScaleError::InvalidArgument);
        }
        for i in 0..MAX_CHROMATONS {
            if let Some(ref mut chr) = df.comp_info[i] {
                chr.packed = false;
                chr.comp_offs = i as u8;
            }
        }
        if debug {
            println!(" [intermediate format {}]", df);
        }
        let res = alloc_video_buffer(NAVideoInfo::new(in_fmt.width, in_fmt.height, false, df), 3);
        if res.is_err() { return Err(ScaleError::AllocError); }
        Ok(res.unwrap())
    }
    fn process(&mut self, pic_in: &NABufferType, pic_out: &mut NABufferType) {
        if let (Some(ref sbuf), Some(ref mut dbuf)) = (pic_in.get_vbuf(), pic_out.get_vbuf()) {
            let istrides = [sbuf.get_stride(0), sbuf.get_stride(1), sbuf.get_stride(2)];
            let dstrides = [dbuf.get_stride(0), dbuf.get_stride(1), dbuf.get_stride(2)];
            let (w, h) = sbuf.get_dimensions(0);
            if sbuf.get_info().get_format().get_num_comp() < 3 {
                return self.process_greyscale(sbuf, dbuf);
            }
            let (sv0, sh0) = sbuf.get_info().get_format().get_chromaton(1).unwrap().get_subsampling();
            let (sv1, sh1) = sbuf.get_info().get_format().get_chromaton(2).unwrap().get_subsampling();

            let uhmask = (1 << sh0) - 1;
            let vhmask = (1 << sh1) - 1;
            let mut roff = dbuf.get_offset(0);
            let mut goff = dbuf.get_offset(1);
            let mut boff = dbuf.get_offset(2);
            let mut yoff = sbuf.get_offset(0);
            let mut uoff = sbuf.get_offset(1);
            let mut voff = sbuf.get_offset(2);
            let src = sbuf.get_data();
            let dst = dbuf.get_data_mut().unwrap();
            if !self.yscale.is_empty() {
                for y in 0..h {
                    for x in 0..w {
                        let y = self.yscale[src[yoff + x] as usize];
                        let u = src[uoff + (x >> sv0)] as usize;
                        let v = src[voff + (x >> sv1)] as usize;
                        let r = y + self.r_chr[v];
                        let g = y + self.g_u[u] + self.g_v[v];
                        let b = y + self.b_chr[u];
                        dst[roff + x] = r.clamp(0, 255) as u8;
                        dst[goff + x] = g.clamp(0, 255) as u8;
                        dst[boff + x] = b.clamp(0, 255) as u8;
                    }
                    roff += dstrides[0];
                    goff += dstrides[1];
                    boff += dstrides[2];
                    yoff += istrides[0];
                    if (y & uhmask) == uhmask {
                        uoff += istrides[1];
                    }
                    if (y & vhmask) == vhmask {
                        voff += istrides[2];
                    }
                }
                return;
            }
            for y in 0..h {
                for x in 0..w {
                    let y = f32::from(src[yoff + x]);
                    let u = f32::from(i16::from(src[uoff + (x >> sv0)]) - 128);
                    let v = f32::from(i16::from(src[voff + (x >> sv1)]) - 128);

                    let (r, g, b) = matrix_mul(&self.matrix, y, u, v);
                    dst[roff + x] = (r as i16).clamp(0, 255) as u8;
                    dst[goff + x] = (g as i16).clamp(0, 255) as u8;
                    dst[boff + x] = (b as i16).clamp(0, 255) as u8;
                }
                roff += dstrides[0];
                goff += dstrides[1];
                boff += dstrides[2];
                yoff += istrides[0];
                if (y & uhmask) == uhmask {
                    uoff += istrides[1];
                }
                if (y & vhmask) == vhmask {
                    voff += istrides[2];
                }
            }
        } else if let (Some(ref sbuf), Some(ref mut dbuf)) = (pic_in.get_vbuf16(), pic_out.get_vbuf()) {
            let istrides = [sbuf.get_stride(0), sbuf.get_stride(1), sbuf.get_stride(2)];
            let dstrides = [dbuf.get_stride(0), dbuf.get_stride(1), dbuf.get_stride(2)];
            let (w, h) = sbuf.get_dimensions(0);
            if sbuf.get_info().get_format().get_num_comp() < 3 {
                return self.process_greyscale16(sbuf, dbuf);
            }
            let yshift = sbuf.get_info().get_format().get_chromaton(0).unwrap().get_depth() - 8;
            let ushift = sbuf.get_info().get_format().get_chromaton(1).unwrap().get_depth() - 8;
            let vshift = sbuf.get_info().get_format().get_chromaton(2).unwrap().get_depth() - 8;
            let (sv0, sh0) = sbuf.get_info().get_format().get_chromaton(1).unwrap().get_subsampling();
            let (sv1, sh1) = sbuf.get_info().get_format().get_chromaton(2).unwrap().get_subsampling();

            let uhmask = (1 << sh0) - 1;
            let vhmask = (1 << sh1) - 1;
            let mut roff = dbuf.get_offset(0);
            let mut goff = dbuf.get_offset(1);
            let mut boff = dbuf.get_offset(2);
            let mut yoff = sbuf.get_offset(0);
            let mut uoff = sbuf.get_offset(1);
            let mut voff = sbuf.get_offset(2);
            let src = sbuf.get_data();
            let dst = dbuf.get_data_mut().unwrap();
            if !self.yscale.is_empty() {
                for y in 0..h {
                    for x in 0..w {
                        let y = self.yscale[usize::from(src[yoff + x] >> yshift)];
                        let u = usize::from(src[uoff + (x >> sv0)] >> ushift);
                        let v = usize::from(src[voff + (x >> sv1)] >> vshift);
                        let r = y + self.r_chr[v];
                        let g = y + self.g_u[u] + self.g_v[v];
                        let b = y + self.b_chr[u];
                        dst[roff + x] = r.clamp(0, 255) as u8;
                        dst[goff + x] = g.clamp(0, 255) as u8;
                        dst[boff + x] = b.clamp(0, 255) as u8;
                    }
                    roff += dstrides[0];
                    goff += dstrides[1];
                    boff += dstrides[2];
                    yoff += istrides[0];
                    if (y & uhmask) == uhmask {
                        uoff += istrides[1];
                    }
                    if (y & vhmask) == vhmask {
                        voff += istrides[2];
                    }
                }
                return;
            }
            for y in 0..h {
                for x in 0..w {
                    let y = f32::from(src[yoff + x]) / f32::from(1u16 << yshift);
                    let u = (i32::from(src[uoff + (x >> sv0)]) - (1 << (ushift - 1))) as f32 / f32::from(1u16 << ushift);
                    let v = (i32::from(src[voff + (x >> sv1)]) - (1 << (vshift - 1))) as f32 / f32::from(1u16 << vshift);

                    let (r, g, b) = matrix_mul(&self.matrix, y, u, v);
                    dst[roff + x] = (r as i16).clamp(0, 255) as u8;
                    dst[goff + x] = (g as i16).clamp(0, 255) as u8;
                    dst[boff + x] = (b as i16).clamp(0, 255) as u8;
                }
                roff += dstrides[0];
                goff += dstrides[1];
                boff += dstrides[2];
                yoff += istrides[0];
                if (y & uhmask) == uhmask {
                    uoff += istrides[1];
                }
                if (y & vhmask) == vhmask {
                    voff += istrides[2];
                }
            }
        } else {
            unimplemented!("unknown yuv2rgb format");
        }
    }
}

impl YuvToRgb {
    fn process_greyscale(&self, sbuf: &NAVideoBuffer<u8>, dbuf: &mut NAVideoBuffer<u8>) {
        let ystride = sbuf.get_stride(0);
        let dstrides = [dbuf.get_stride(0), dbuf.get_stride(1), dbuf.get_stride(2)];
        let (w, h) = sbuf.get_dimensions(0);
        let mut roff = dbuf.get_offset(0);
        let mut goff = dbuf.get_offset(1);
        let mut boff = dbuf.get_offset(2);
        let mut yoff = sbuf.get_offset(0);
        let src = sbuf.get_data();
        let dst = dbuf.get_data_mut().unwrap();
        if !self.yscale.is_empty() {
            for _y in 0..h {
                for x in 0..w {
                    let y = self.yscale[src[yoff + x] as usize];
                    let r = y + self.r_chr[128];
                    let g = y + self.g_u[128] + self.g_v[128];
                    let b = y + self.b_chr[128];
                    dst[roff + x] = r.clamp(0, 255) as u8;
                    dst[goff + x] = g.clamp(0, 255) as u8;
                    dst[boff + x] = b.clamp(0, 255) as u8;
                }
                roff += dstrides[0];
                goff += dstrides[1];
                boff += dstrides[2];
                yoff += ystride;
            }
        } else {
            for _y in 0..h {
                for x in 0..w {
                    let y = f32::from(src[yoff + x]);
                    let (r, g, b) = matrix_mul(&self.matrix, y, 0.0, 0.0);
                    dst[roff + x] = (r as i16).clamp(0, 255) as u8;
                    dst[goff + x] = (g as i16).clamp(0, 255) as u8;
                    dst[boff + x] = (b as i16).clamp(0, 255) as u8;
                }
                roff += dstrides[0];
                goff += dstrides[1];
                boff += dstrides[2];
                yoff += ystride;
            }
        }
    }
    fn process_greyscale16(&self, sbuf: &NAVideoBuffer<u16>, dbuf: &mut NAVideoBuffer<u8>) {
        let ystride = sbuf.get_stride(0);
        let dstrides = [dbuf.get_stride(0), dbuf.get_stride(1), dbuf.get_stride(2)];
        let yshift = sbuf.get_info().get_format().get_chromaton(0).unwrap().get_depth() - 8;
        let (w, h) = sbuf.get_dimensions(0);
        let mut roff = dbuf.get_offset(0);
        let mut goff = dbuf.get_offset(1);
        let mut boff = dbuf.get_offset(2);
        let mut yoff = sbuf.get_offset(0);
        let src = sbuf.get_data();
        let dst = dbuf.get_data_mut().unwrap();
        if !self.yscale.is_empty() {
            for _y in 0..h {
                for x in 0..w {
                    let y = self.yscale[usize::from(src[yoff + x] >> yshift)];
                    let r = y + self.r_chr[128];
                    let g = y + self.g_u[128] + self.g_v[128];
                    let b = y + self.b_chr[128];
                    dst[roff + x] = r.clamp(0, 255) as u8;
                    dst[goff + x] = g.clamp(0, 255) as u8;
                    dst[boff + x] = b.clamp(0, 255) as u8;
                }
                roff += dstrides[0];
                goff += dstrides[1];
                boff += dstrides[2];
                yoff += ystride;
            }
        } else {
            for _y in 0..h {
                for x in 0..w {
                    let y = f32::from(src[yoff + x]);
                    let (r, g, b) = matrix_mul(&self.matrix, y, 0.0, 0.0);
                    dst[roff + x] = (r as i16).clamp(0, 255) as u8;
                    dst[goff + x] = (g as i16).clamp(0, 255) as u8;
                    dst[boff + x] = (b as i16).clamp(0, 255) as u8;
                }
                roff += dstrides[0];
                goff += dstrides[1];
                boff += dstrides[2];
                yoff += ystride;
            }
        }
    }
}

pub fn create_yuv2rgb() -> Box<dyn Kernel> {
    Box::new(YuvToRgb::new())
}
