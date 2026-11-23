use crate::formats::*;
use super::*;
use super::kernel::Kernel;

#[derive(Default,Clone,Copy,PartialEq,Debug)]
pub struct Pixel {
    pub r: u8,
    pub g: u8,
    pub b: u8,
}

impl Pixel {
    #[allow(dead_code)]
    fn new(src: &[u8]) -> Self {
        Self { r: src[0], g: src[1], b: src[2] }
    }
    fn to_rgb(self) -> [u8; 3] {
        [self.r, self.g, self.b]
    }
    fn dist(&self, pix: Pixel) -> u32 {
        let dr = i32::from(self.r) - i32::from(pix.r);
        let dg = i32::from(self.g) - i32::from(pix.g);
        let db = i32::from(self.b) - i32::from(pix.b);
        (dr * dr + dg * dg + db * db) as u32
    }
    fn min(&self, pix: Pixel) -> Pixel {
        Pixel { r: self.r.min(pix.r), g: self.g.min(pix.g), b: self.b.min(pix.b) }
    }
    fn max(&self, pix: Pixel) -> Pixel {
        Pixel { r: self.r.max(pix.r), g: self.g.max(pix.g), b: self.b.max(pix.b) }
    }
}

#[allow(dead_code)]
fn avg_u8(a: u8, b: u8) -> u8 {
    (a & b) + ((a ^ b) >> 1)
}

mod elbg;
mod mediancut;
mod neuquant;
mod palettise;

//use elbg::ELBG;
//use mediancut::quantise_median_cut;
//use neuquant::NeuQuantQuantiser;

#[derive(Clone,Copy,Debug,PartialEq,Default)]
/// Palette quantisation algorithms.
pub enum QuantisationMode {
    /// Median cut approach proposed by Paul Heckbert.
    ///
    /// This is moderately fast and moderately good.
    #[default]
    MedianCut,
    /// Enhanced LBG algorithm proposed by Giuseppe Patane and Marco Russo.
    ///
    /// This is slow but good method.
    ELBG,
    /// NeuQuant algorithm proposed by Anthony Dekker.
    ///
    /// It may operate on randomly subsampled image with subsampling factors 1-30.
    /// This algorithm is fast especially with high subsampling factor but output palette may be far from optimal one.
    NeuQuant(u8),
}

#[derive(Clone,Copy,Debug,PartialEq,Default)]
/// Algorithms for seaching an appropriate palette entry for a given pixel.
pub enum PaletteSearchMode {
    /// Full search (slowest).
    Full,
    /// Local search (faster but may be not so good).
    #[default]
    Local,
    /// k-d tree based one (the fastest but not so accurate).
    KDTree,
}

use crate::scale::palette::elbg::ELBG;
use crate::scale::palette::mediancut::quantise_median_cut;
use crate::scale::palette::neuquant::NeuQuantQuantiser;
use crate::scale::palette::palettise::*;

fn palettise_frame_internal(pic_in: &NABufferType, pic_out: &mut NABufferType, qmode: QuantisationMode, palmode: PaletteSearchMode, pixels: &mut Vec<Pixel>) -> ScaleResult<()> {
    if let (Some(ref sbuf), Some(ref mut dbuf)) = (pic_in.get_vbuf(), pic_out.get_vbuf()) {
        let ioff = sbuf.get_offset(0);
        let (w, h) = sbuf.get_dimensions(0);
        let istride = sbuf.get_stride(0);
        let ifmt = sbuf.get_info().get_format();
        let sdata1 = sbuf.get_data();
        let sdata = &sdata1[ioff..];

        let doff = dbuf.get_offset(0);
        let paloff = dbuf.get_offset(1);
        let dstride = dbuf.get_stride(0);
        let ofmt = dbuf.get_info().get_format();
        let dst = dbuf.get_data_mut().unwrap();

        pixels.clear();
        if !ifmt.is_unpacked() {
            let esize = ifmt.elem_size as usize;
            let coffs = [ifmt.comp_info[0].unwrap().comp_offs as usize, ifmt.comp_info[1].unwrap().comp_offs as usize, ifmt.comp_info[2].unwrap().comp_offs as usize];
            for src in sdata.chunks(istride).take(h) {
                for chunk in src.chunks_exact(esize).take(w) {
                    let pixel = Pixel{ r: chunk[coffs[0]], g: chunk[coffs[1]], b: chunk[coffs[2]] };
                    pixels.push(pixel);
                }
            }
        } else {
            let mut roff = ioff;
            let mut goff = sbuf.get_offset(1);
            let mut boff = sbuf.get_offset(2);
            let rstride = istride;
            let gstride = sbuf.get_stride(1);
            let bstride = sbuf.get_stride(2);
            for _ in 0..h {
                for x in 0..w {
                    let pixel = Pixel{ r: sdata[roff + x], g: sdata[goff + x], b: sdata[boff + x] };
                    pixels.push(pixel);
                }
                roff += rstride;
                goff += gstride;
                boff += bstride;
            }
        }
        let mut pal = [[0u8; 3]; 256];
        match qmode {
            QuantisationMode::ELBG => {
                let mut elbg = ELBG::new_random();
                elbg.quantise(pixels.as_slice(), &mut pal);
            },
            QuantisationMode::MedianCut => {
                quantise_median_cut(pixels.as_slice(), &mut pal);
            },
            QuantisationMode::NeuQuant(fact) => {
                let mut nq = NeuQuantQuantiser::new(fact as usize);
                nq.learn(pixels.as_slice());
                nq.make_pal(&mut pal);
            },
        };
        let esize = ofmt.elem_size as usize;
        let coffs = [ofmt.comp_info[0].unwrap().comp_offs as usize, ofmt.comp_info[1].unwrap().comp_offs as usize, ofmt.comp_info[2].unwrap().comp_offs as usize];
        for (dpal, spal) in dst[paloff..].chunks_mut(esize).zip(pal.iter()) {
            dpal[coffs[0]] = spal[0];
            dpal[coffs[1]] = spal[1];
            dpal[coffs[2]] = spal[2];
        }

        let dst = &mut dst[doff..];
        match palmode {
            PaletteSearchMode::Full => {
                for (dline, sline) in dst.chunks_mut(dstride).take(h).zip(pixels.chunks(w)) {
                    for (didx, pix) in dline.iter_mut().take(w).zip(sline.iter()) {
                        let rgb = pix.to_rgb();
                        *didx = find_nearest(&rgb, &pal) as u8;
                    }
                }
            },
            PaletteSearchMode::Local => {
                let ls = LocalSearch::new(&pal);
                for (dline, sline) in dst.chunks_mut(dstride).take(h).zip(pixels.chunks(w)) {
                    for (didx, pix) in dline.iter_mut().take(w).zip(sline.iter()) {
                        *didx = ls.search(pix.to_rgb()) as u8;
                    }
                }
            },
            PaletteSearchMode::KDTree => {
                let kdtree = KDTree::new(&pal);
                for (dline, sline) in dst.chunks_mut(dstride).take(h).zip(pixels.chunks(w)) {
                    for (didx, pix) in dline.iter_mut().take(w).zip(sline.iter()) {
                        *didx = kdtree.search(pix.to_rgb()) as u8;
                    }
                }
            },
        };
        Ok(())
    } else {
        Err(ScaleError::InvalidArgument)
    }
}

/// Converts packed RGB frame into palettised one.
///
/// This function can operate in several modes of both palette generation and colour substitution with palette indices.
/// Some may work fast but produce worse output image.
/// See [`QuantisationMode`] and [`PaletteSearchMode`] for details.
/// If you are not sure what to use there are `QuantisationMode::default()` and `PaletteSearchMode::default()`.
///
/// [`QuantisationMode`]: ./enum.QuantisationMode.html
/// [`PaletteSearchMode`]: ./enum.PaletteSearchMode.html
pub fn palettise_frame(pic_in: &NABufferType, pic_out: &mut NABufferType, qmode: QuantisationMode, palmode: PaletteSearchMode) -> ScaleResult<()> {
    let size;
    if let Some(ref vbuf) = pic_in.get_vbuf() {
//todo check format for being packed RGB in and pal out
        let (w, h) = vbuf.get_dimensions(0);
        size = w * h;
    } else {
        return Err(ScaleError::InvalidArgument);
    }
    let mut pixels = Vec::with_capacity(size);
    palettise_frame_internal(pic_in, pic_out, qmode, palmode, &mut pixels)
}


#[derive(Default)]
struct PalettiseKernel {
    pixels:     Vec<Pixel>,
    qmode:      QuantisationMode,
    palmode:    PaletteSearchMode,
}

impl PalettiseKernel {
    fn new() -> Self { Self::default() }
}

impl Kernel for PalettiseKernel {
    fn init(&mut self, in_fmt: &ScaleInfo, _dest_fmt: &ScaleInfo, options: &[(String, String)]) -> ScaleResult<NABufferType> {
        for (name, value) in options.iter() {
            match name.as_str() {
                "pal.quant" => {
                    self.qmode = match value.as_str() {
                            "mediancut" => QuantisationMode::MedianCut,
                            "elbg"      => QuantisationMode::ELBG,
                            "neuquant"  => QuantisationMode::NeuQuant(3),
                            _           => QuantisationMode::default(),
                        };
                },
                "pal.search" => {
                    self.palmode = match value.as_str() {
                            "full"      => PaletteSearchMode::Full,
                            "local"     => PaletteSearchMode::Local,
                            "kdtree"    => PaletteSearchMode::KDTree,
                            _           => PaletteSearchMode::default(),
                        };
                },
                _ => {},
            };
        }

        self.pixels = Vec::with_capacity(in_fmt.width * in_fmt.height);
        let res = alloc_video_buffer(NAVideoInfo::new(in_fmt.width, in_fmt.height, false, PAL8_FORMAT), 0);
        if res.is_err() { return Err(ScaleError::AllocError); }
        Ok(res.unwrap())
    }
    fn process(&mut self, pic_in: &NABufferType, pic_out: &mut NABufferType) {
        palettise_frame_internal(pic_in, pic_out, self.qmode, self.palmode, &mut self.pixels).unwrap();
    }
}

pub fn create_palettise() -> Box<dyn Kernel> {
    Box::new(PalettiseKernel::new())
}
