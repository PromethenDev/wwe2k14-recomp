use nihav_core::codecs::*;
use nihav_core::io::bitwriter::*;

use super::binkviddata::*;

mod bundle;
use bundle::*;
mod dsp;
use dsp::*;
mod mc;
use mc::*;
mod rc;
use rc::*;

const THRESHOLD: u32 = 64;
const MAX_DIST: u32 = u32::MAX;

#[derive(Clone,Copy,Default,PartialEq)]
enum DoublingMode {
    #[default]
    None,
    Height2X,
    HeightIlace,
    Scale2X,
    Width2X,
    Width2XIlace
}

impl std::fmt::Display for DoublingMode {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> Result<(), std::fmt::Error> {
        match *self {
            DoublingMode::None => write!(f, "none"),
            DoublingMode::Height2X => write!(f, "height2x"),
            DoublingMode::HeightIlace => write!(f, "height_il"),
            DoublingMode::Scale2X => write!(f, "scale2x"),
            DoublingMode::Width2X => write!(f, "width2x"),
            DoublingMode::Width2XIlace => write!(f, "width2x_il"),
        }
    }
}

#[derive(Clone,Copy,Debug,PartialEq)]
enum BlockMode {
    Skip,
    Scaled,
    Run,
    Intra,
    Residue,
    Inter,
    Fill,
    Pattern,
    Motion,
    Raw,
}

const BLOCK_MODE_NAMES: &[&str] = &[
    "skip", "run", "intra", "residue", "inter", "fill", "pattern", "motion", "raw", "scaled"
];

impl From<BlockMode> for usize {
    fn from(bmode: BlockMode) -> usize {
        match bmode {
            BlockMode::Skip => 0,
            BlockMode::Run => 1,
            BlockMode::Intra => 2,
            BlockMode::Residue => 3,
            BlockMode::Inter => 4,
            BlockMode::Fill => 5,
            BlockMode::Pattern => 6,
            BlockMode::Motion => 7,
            BlockMode::Raw => 8,
            BlockMode::Scaled => 9,
        }
    }
}

impl std::str::FromStr for BlockMode {
    type Err = ();
    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s {
            "skip"      => Ok(BlockMode::Skip),
            "run"       => Ok(BlockMode::Run),
            "intra"     => Ok(BlockMode::Intra),
            "residue"   => Ok(BlockMode::Residue),
            "inter"     => Ok(BlockMode::Inter),
            "fill"      => Ok(BlockMode::Fill),
            "pattern"   => Ok(BlockMode::Pattern),
            "motion"    => Ok(BlockMode::Motion),
            "raw"       => Ok(BlockMode::Raw),
            "scaled"    => Ok(BlockMode::Scaled),
            _ => Err(()),
        }
    }
}

impl std::fmt::Display for BlockMode {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> Result<(), std::fmt::Error> {
        write!(f, "{}", BLOCK_MODE_NAMES[usize::from(*self)])
    }
}

impl BlockMode {
    fn to_code_b(self) -> u8 {
        match self {
            BlockMode::Skip => 0,
            BlockMode::Run => 1,
            BlockMode::Intra => 2,
            BlockMode::Residue => 3,
            BlockMode::Inter => 4,
            BlockMode::Fill => 5,
            BlockMode::Pattern => 6,
            BlockMode::Motion => 7,
            BlockMode::Raw => 8,
            _ => unreachable!(),
        }
    }
}

fn close_enough(base: u8, thr: u8, pix: u8) -> bool {
    pix >= base.saturating_sub(thr) && pix <= base.saturating_add(thr)
}

fn write_run(tokens: &mut BlockTokens, pos: usize, len: usize, is_run: bool, clrs: &[u8; 64], is_b: bool) {
    tokens.other.push((is_run as u16, 1));
    if is_b {
        tokens.other.push(((len - 1) as u16, BINKB_RUN_BITS[pos]));
    } else {
        unimplemented!();
    }
    if is_run {
        tokens.colors.push(clrs[pos]);
    } else {
        tokens.colors.extend_from_slice(&clrs[pos..][..len]);
    }
}

fn find_run_pattern(cur_blk: &[u8; 64], tokens: &mut BlockTokens, tmp_tok: &mut BlockTokens, is_b: bool, thr: u8) -> u32 {
    tokens.clear();
    let mut best_diff = MAX_DIST;
    let mut clrs = [0; 64];
    for (id, pattern) in BINK_PATTERNS.iter().enumerate() {
        tmp_tok.clear();
        tmp_tok.other.push((id as u16, 4));

        for (dst, &idx) in clrs.iter_mut().zip(pattern.iter()) {
            *dst = cur_blk[usize::from(idx)];
        }

        let mut cur_diff = 0;
        let mut last_val = 0;
        let mut last_pos = 0;
        let mut len = 0;
        let mut is_run = false;

        for (i, &val) in clrs.iter().enumerate() {
            if len > 0 && close_enough(last_val, thr, val) {
                if is_run || len == 1 {
                    is_run = true;
                    len += 1;
                } else {
                    write_run(tmp_tok, last_pos, len - 1, is_run, &clrs, is_b);
                    last_pos = i - 1;
                    last_val = clrs[last_pos];
                    len = 2;
                }
                cur_diff += pix_dist(last_val, val);
            } else {
                if len > 0 {
                    write_run(tmp_tok, last_pos, len, is_run, &clrs, is_b);
                }
                last_pos = i;
                last_val = val;
                len = 1;
                is_run = false;
            }
        }
        match len {
            0 => {},
            1 => tmp_tok.colors.push(last_val),
            _ => write_run(tmp_tok, last_pos, len, is_run, &clrs, is_b),
        };

        if cur_diff < best_diff {
            best_diff = cur_diff;
            std::mem::swap(tokens, tmp_tok);
        }
    }
    best_diff
}

fn find_pattern(cur_blk: &[u8; 64], tokens: &mut BlockTokens) -> u32 {
    let sum = cur_blk.iter().fold(0u16,
                    |acc, &a| acc + u16::from(a));
    let avg = ((sum + 32) >> 6) as u8;
    let mut sum_ba = 0u16;
    let mut sum_aa = 0u16;
    let mut cnt_ba = 0u16;
    let mut cnt_aa = 0u16;
    for &pix in cur_blk.iter() {
        if pix < avg {
            sum_ba += u16::from(pix);
            cnt_ba += 1;
        } else {
            sum_aa += u16::from(pix);
            cnt_aa += 1;
        }
    }
    if cnt_ba > 0 { // not flat
        let clr_ba = ((sum_ba + cnt_ba / 2) / cnt_ba) as u8;
        let clr_aa = ((sum_aa + cnt_aa / 2) / cnt_aa) as u8;
        tokens.clear();
        tokens.colors.push(clr_ba);
        tokens.colors.push(clr_aa);
        let mut diff = 0;
        for row in cur_blk.chunks_exact(8) {
            let mut pat = 0;
            let mut mask = 1;
            for &p in row.iter() {
                if p < avg {
                    diff += pix_dist(p, clr_ba);
                } else {
                    pat |= mask;
                    diff += pix_dist(p, clr_aa);
                }
                mask <<= 1;
            }
            tokens.pattern.push(pat);
        }
        diff
    } else {
        MAX_DIST
    }
}

struct BinkEncoder {
    stream:         Option<NAStreamRef>,
    pkt:            Option<NAPacket>,
    cur_frm:        NAVideoBufferRef<u8>,
    last_frm:       NAVideoBufferRef<u8>,
    scale_mode:     DoublingMode,
    version:        char,
    bundles:        Bundles,
    nframes:        u64,
    frame_no:       u64,

    bst_tokens:     BlockTokens,
    tmp_tokens:     BlockTokens,
    tmp_tok2:       BlockTokens,

    dsp:            DSP,
    rc:             RateControl,

    forbidden:      [bool; 12],

    print_stats:    bool,
    blk_stats:      [usize; 12],
    tot_size:       usize,
    iq_stats:       [usize; 16],
    pq_stats:       [usize; 16],
}

impl BinkEncoder {
    fn new() -> Self {
        let frm = alloc_video_buffer(NAVideoInfo::new(4, 4, false, YUV420_FORMAT), 0).unwrap();
        let cur_frm = frm.get_vbuf().unwrap();
        let last_frm = cur_frm.clone();
        Self {
            stream:         None,
            pkt:            None,
            cur_frm, last_frm,
            scale_mode:     DoublingMode::default(),
            version:        'b',
            bundles:        Bundles::default(),
            nframes:        0,
            frame_no:       0,

            bst_tokens:     BlockTokens::new(),
            tmp_tokens:     BlockTokens::new(),
            tmp_tok2:       BlockTokens::new(),

            dsp:            DSP::new(),
            rc:             RateControl::new(),

            forbidden:      [false; 12],

            print_stats:    false,
            blk_stats:      [0; 12],
            tot_size:       0,
            iq_stats:       [0; 16],
            pq_stats:       [0; 16],
        }
    }
    fn encode_frame(&mut self, bw: &mut BitWriter, srcbuf: &NAVideoBuffer<u8>) -> EncoderResult<bool> {
        let frm = NASimpleVideoFrame::from_video_buf(&mut self.cur_frm).unwrap();
        let src = srcbuf.get_data();
        let last = self.last_frm.get_data();

        let is_b = self.version == 'b';
        let first_frame = self.frame_no == 1;
        let pat_run_thr = self.rc.pattern_run_threshold();

        let mut cur_blk = [0; 64];
        let mut mv_blk = [0; 64];
        let mut is_intra = true;
        let mut cur_forbidden = self.forbidden;
        self.rc.modify_forbidden_btypes(&mut cur_forbidden);
        self.dsp.set_quant_ranges(self.rc.get_quant_ranges());
        for plane in 0..3 {
            let loff = self.last_frm.get_offset(plane);
            let soff = srcbuf.get_offset(plane);
            let doff = frm.offset[plane];
            let lstride = self.last_frm.get_stride(plane);
            let sstride = srcbuf.get_stride(plane);
            let dstride = frm.stride[plane];

            let cur_w = (frm.width[plane] + 7) & !7;
            let cur_h = (frm.height[plane] + 7) & !7;
            let dst = &mut frm.data[doff..];
            let src = &src[soff..];
            let last = &last[loff..];

            if is_b {
                // copy last frame as motion search is performed on partially updated frame
                for (dline, sline) in dst.chunks_mut(dstride).zip(last.chunks(lstride)).take(cur_h) {
                    dline[..cur_w].copy_from_slice(&sline[..cur_w]);
                }
            }

            self.bundles.reset();
            for (row, stripe) in src.chunks(sstride * 8).take(cur_h / 8).enumerate() {
                self.bundles.new_row(row);
                let y = row * 8;
                for x in (0..cur_w).step_by(8) {
                    for (dst, src) in cur_blk.chunks_exact_mut(8).zip(stripe[x..].chunks(sstride)) {
                        dst.copy_from_slice(&src[..8]);
                    }

                    let (skip_dist, skip_diff) = if !first_frame && !cur_forbidden[usize::from(BlockMode::Skip)] {
                            let diff = calc_diff(&cur_blk, 8, &last[x + y * lstride..], lstride);
                            (self.rc.metric(diff, 0), diff)
                        } else { // no skip blocks for the first frame
                            (MAX_DIST, MAX_DIST)
                        };
                    let mut block_mode = BlockMode::Skip;
                    let mut best_dist = skip_dist;
                    self.bst_tokens.clear();

                    if best_dist > THRESHOLD && !first_frame && !cur_forbidden[usize::from(BlockMode::Motion)] {
                        let (mv, diff) = if is_b {
                                mv_search(dst, dstride, cur_w, cur_h, x, y, skip_diff, &cur_blk, &mut mv_blk)
                            } else {
                                mv_search(last, lstride, cur_w, cur_h, x, y, skip_diff, &cur_blk, &mut mv_blk)
                            };
                        if is_b {
                            get_block(dst, dstride, x, y, mv, &mut mv_blk);
                        } else {
                            get_block(last, lstride, x, y, mv, &mut mv_blk);
                        }
                        self.tmp_tokens.clear();
                        self.tmp_tokens.xoff.push(mv.x as i8);
                        self.tmp_tokens.yoff.push(mv.y as i8);
                        let mv_dist = self.rc.metric(diff, self.tmp_tokens.bits(is_b));
                        if mv_dist < best_dist {
                            block_mode = BlockMode::Motion;
                            best_dist = mv_dist;
                            std::mem::swap(&mut self.bst_tokens, &mut self.tmp_tokens);
                        }
                        self.dsp.get_diff(&mv_blk, &cur_blk);
                        if best_dist > THRESHOLD && !cur_forbidden[usize::from(BlockMode::Residue)] {
                            self.tmp_tokens.clear();
                            self.tmp_tokens.xoff.push(mv.x as i8);
                            self.tmp_tokens.yoff.push(mv.y as i8);
                            let diff = self.dsp.try_residue(&mut self.tmp_tokens);
                            let res_dist = self.rc.metric(diff, self.tmp_tokens.bits(is_b));
                            if res_dist < best_dist {
                                best_dist = res_dist;
                                block_mode = BlockMode::Residue;
                                std::mem::swap(&mut self.bst_tokens, &mut self.tmp_tokens);
                            }
                        }
                        if best_dist > THRESHOLD && !cur_forbidden[usize::from(BlockMode::Inter)] {
                            self.tmp_tokens.clear();
                            self.tmp_tokens.xoff.push(mv.x as i8);
                            self.tmp_tokens.yoff.push(mv.y as i8);
                            let dct_p_dist = self.dsp.try_dct_inter(&mv_blk, &cur_blk, &mut self.tmp_tokens, &mut self.tmp_tok2, is_b, &self.rc, best_dist);
                            if dct_p_dist < best_dist {
                                best_dist = dct_p_dist;
                                block_mode = BlockMode::Inter;
                                std::mem::swap(&mut self.bst_tokens, &mut self.tmp_tokens);
                            }
                        }
                    }
                    if best_dist > THRESHOLD && !cur_forbidden[usize::from(BlockMode::Fill)] {
                        let sum = cur_blk.iter().fold(0u16,
                                        |acc, &a| acc + u16::from(a));
                        let avg = ((sum + 32) >> 6) as u8;
                        self.tmp_tokens.clear();
                        self.tmp_tokens.colors.push(avg);
                        let diff = cur_blk.iter().fold(0u32,
                                            |acc, &a| acc + pix_dist(a, avg));
                        let fill_dist = self.rc.metric(diff, self.tmp_tokens.bits(is_b));
                        if fill_dist < best_dist {
                            block_mode = BlockMode::Fill;
                            best_dist = fill_dist;
                            std::mem::swap(&mut self.bst_tokens, &mut self.tmp_tokens);
                        }
                    }
                    if best_dist > THRESHOLD && !cur_forbidden[usize::from(BlockMode::Pattern)] {
                        let diff = find_pattern(&cur_blk, &mut self.tmp_tokens);
                        let pat_dist = self.rc.metric(diff, self.tmp_tokens.bits(is_b));
                        if pat_dist < best_dist {
                            best_dist = pat_dist;
                            block_mode = BlockMode::Pattern;
                            std::mem::swap(&mut self.bst_tokens, &mut self.tmp_tokens);
                        }
                    }
                    if best_dist > THRESHOLD && !cur_forbidden[usize::from(BlockMode::Run)] {
                        let diff = find_run_pattern(&cur_blk, &mut self.tmp_tokens, &mut self.tmp_tok2, is_b, pat_run_thr);
                        let run_dist = self.rc.metric(diff, self.tmp_tokens.bits(is_b));
                        if run_dist < best_dist {
                            best_dist = run_dist;
                            block_mode = BlockMode::Run;
                            std::mem::swap(&mut self.bst_tokens, &mut self.tmp_tokens);
                        }
                    }
                    if best_dist > THRESHOLD && !cur_forbidden[usize::from(BlockMode::Intra)] {
                        let dct_i_dist = self.dsp.try_dct_intra(&cur_blk, &mut self.tmp_tokens, &mut self.tmp_tok2, is_b, &self.rc, best_dist);
                        if dct_i_dist < best_dist {
                            best_dist = dct_i_dist;
                            block_mode = BlockMode::Intra;
                            std::mem::swap(&mut self.bst_tokens, &mut self.tmp_tokens);
                        }
                    }
                    if best_dist > THRESHOLD && !cur_forbidden[usize::from(BlockMode::Raw)]
                            && (!is_b || self.bundles.can_fit_raw_block()) {
                        self.tmp_tokens.clear();
                        self.tmp_tokens.colors.extend_from_slice(&cur_blk);
                        let raw_dist = self.rc.metric(0, self.tmp_tokens.bits(is_b));
                        if raw_dist < best_dist {
                            best_dist = raw_dist;
                            block_mode = BlockMode::Raw;
                            std::mem::swap(&mut self.bst_tokens, &mut self.tmp_tokens);
                        }
                    }
                    let _ = best_dist; // silence a warning

                    let bmode = if is_b { block_mode.to_code_b() } else { unimplemented!() };
                    self.bundles.add_block_type(bmode);
                    self.bundles.add_tokens(&self.bst_tokens);

                    self.blk_stats[usize::from(block_mode)] += 1;

                    match block_mode {
                        BlockMode::Skip if is_b => {},
                        BlockMode::Skip => {
                            for (dline, sline) in dst[x + y * dstride..].chunks_mut(dstride)
                                    .zip(last[x + y * lstride..].chunks(lstride)).take(8) {
                                dline[..8].copy_from_slice(&sline[..8]);
                            }
                            is_intra = false;
                        }
                        BlockMode::Fill => {
                            let avg = self.bst_tokens.colors[0];
                            for dline in dst[x + y * dstride..].chunks_mut(dstride).take(8) {
                                for el in dline[..8].iter_mut() {
                                    *el = avg;
                                }
                            }
                        },
                        BlockMode::Pattern => {
                            for (&pat, dline) in self.bst_tokens.pattern.iter().zip(dst[x + y * dstride..].chunks_mut(dstride)) {
                                let mut pattern = pat as usize;
                                for el in dline[..8].iter_mut() {
                                    *el = self.bst_tokens.colors[pattern & 1];
                                    pattern >>= 1;
                                }
                            }
                        },
                        BlockMode::Run => {
                            let mut clrs = self.bst_tokens.colors.iter();
                            let mut data = self.bst_tokens.other.iter();
                            let &(idx, _) = data.next().unwrap();
                            let pattern = BINK_PATTERNS[usize::from(idx)];
                            let mut len = 0;
                            let mut is_run = false;
                            let mut run_val = 0;
                            for (i, &idx) in pattern.iter().enumerate() {
                                let dst_idx = (idx & 7) as usize + ((idx >> 3) as usize) * dstride;
                                if len == 0 {
                                    if i < 63 {
                                        let &(flag, _nbits) = data.next().unwrap();
assert_eq!(_nbits, 1);
                                        is_run = flag != 0;
                                        let &(len1, _) = data.next().unwrap();
                                        len = usize::from(len1) + 1;
                                        if is_run {
                                            run_val = *clrs.next().unwrap();
                                        }
                                    } else {
                                        len = 1;
                                        is_run = false;
                                    }
                                }
                                dst[x + y * dstride + dst_idx] = if is_run {
                                        run_val
                                    } else {
                                        *clrs.next().unwrap()
                                    };
                                len -= 1;
                            }
                        },
                        BlockMode::Raw => {
                            put_block(&mut dst[x + y * dstride..], dstride, &cur_blk);
                        },
                        BlockMode::Motion => {
                            put_block(&mut dst[x + y * dstride..], dstride, &mv_blk);
                            is_intra = false;
                        },
                        BlockMode::Residue => {
                            self.dsp.recon_residue(&mut dst[x + y * dstride..], dstride, &mv_blk);
                            is_intra = false;
                        },
                        BlockMode::Inter => {
                            self.dsp.recon_dct_p(&mut dst[x + y * dstride..], dstride);
                            let q = if is_b {
                                    usize::from(self.bst_tokens.interq[0])
                                } else {
                                    let (qval, _) = self.bst_tokens.other[self.bst_tokens.other.len() - 1];
                                    usize::from(qval)
                                };
                            self.pq_stats[q] += 1;
                            is_intra = false;
                        },
                        BlockMode::Intra => {
                            self.dsp.recon_dct_i(&mut dst[x + y * dstride..], dstride);
                            let q = if is_b {
                                    usize::from(self.bst_tokens.intraq[0])
                                } else {
                                    let (qval, _) = self.bst_tokens.other[self.bst_tokens.other.len() - 1];
                                    usize::from(qval)
                                };
                            self.iq_stats[q] += 1;
                        },
                        _ => unimplemented!(),
                    };
                }
                self.bundles.end_row();
            }
            for row in 0..(cur_h / 8) {
                self.bundles.write(bw, row);
            }
            while (bw.tell() & 0x1F) != 0 {
                bw.write0();
            }
        }
        Ok(is_intra)
    }
    fn encode_skip(&mut self, bw: &mut BitWriter) -> EncoderResult<()> {
        let src = self.last_frm.get_data();
        let dst = self.cur_frm.get_data_mut().unwrap();
        dst.copy_from_slice(src);

        for plane in 0..3 {
            let (width, height) = self.cur_frm.get_dimensions(plane);
            let tiles_w = (width  + 7) >> 3;
            let tiles_h = (height + 7) >> 3;
            self.bundles.reset();
            for row in 0..tiles_h {
                self.bundles.new_row(row);
                for _ in 0..tiles_w {
                    self.bundles.add_block_type(0); // skip always has code 0
                }
                self.bundles.end_row();
            }
            for row in 0..tiles_h {
                self.bundles.write(bw, row);
            }
        }

        Ok(())
    }
}

impl NAEncoder for BinkEncoder {
    fn negotiate_format(&self, encinfo: &EncodeParameters) -> EncoderResult<EncodeParameters> {
        match encinfo.format {
            NACodecTypeInfo::None => {
                Ok(EncodeParameters {
                    format: NACodecTypeInfo::Video(NAVideoInfo::new(0, 0, true, YUV420_FORMAT)),
                    ..Default::default()
                })
            },
            NACodecTypeInfo::Audio(_) => Err(EncoderError::FormatError),
            NACodecTypeInfo::Video(vinfo) => {
                let outinfo = NAVideoInfo::new(vinfo.width, vinfo.height, false, YUV420_FORMAT);
                let mut ofmt = *encinfo;
                ofmt.format = NACodecTypeInfo::Video(outinfo);
                Ok(ofmt)
            }
        }
    }
    fn get_capabilities(&self) -> u64 { ENC_CAPS_SKIPFRAME }
    fn init(&mut self, stream_id: u32, encinfo: EncodeParameters) -> EncoderResult<NAStreamRef> {
        match encinfo.format {
            NACodecTypeInfo::None => Err(EncoderError::FormatError),
            NACodecTypeInfo::Audio(_) => Err(EncoderError::FormatError),
            NACodecTypeInfo::Video(vinfo) => {
                if vinfo.format != YUV420_FORMAT {
                    return Err(EncoderError::FormatError);
                }

                let mut edata = vec![b'B', b'I', b'K', self.version as u8, 0, 0, 0, 0];
                match self.scale_mode {
                    DoublingMode::None => {},
                    DoublingMode::Height2X => {
                        edata[7] |= 0x10;
                    },
                    DoublingMode::HeightIlace => {
                        edata[7] |= 0x20;
                    },
                    DoublingMode::Width2X => {
                        edata[7] |= 0x30;
                    },
                    DoublingMode::Scale2X => {
                        edata[7] |= 0x40;
                    },
                    DoublingMode::Width2XIlace => {
                        edata[7] |= 0x50;
                    },
                };

                if self.nframes == 0 {
                    println!("Bink should set the number of frames in the stream");
                    return Err(EncoderError::FormatError);
                }

                let out_info = NAVideoInfo::new(vinfo.width, vinfo.height, false, YUV420_FORMAT);
                let info = NACodecInfo::new("bink-video", NACodecTypeInfo::Video(out_info), Some(edata));
                let mut stream = NAStream::new(StreamType::Video, stream_id, info, encinfo.tb_num, encinfo.tb_den, self.nframes);
                stream.set_num(stream_id as usize);
                let stream = stream.into_ref();

                self.stream = Some(stream.clone());

                let frm = alloc_video_buffer(out_info, 4)?;
                self.cur_frm  = frm.get_vbuf().unwrap();
                let frm = alloc_video_buffer(out_info, 4)?;
                self.last_frm = frm.get_vbuf().unwrap();

                let cdata = self.cur_frm.get_data_mut().unwrap();
                for el in cdata.iter_mut() {
                    *el = 0x00;
                }
                let cdata = self.last_frm.get_data_mut().unwrap();
                for el in cdata.iter_mut() {
                    *el = 0x00;
                }

                self.rc.init(encinfo.tb_num, encinfo.tb_den, encinfo.bitrate, encinfo.quality);

                Ok(stream)
            },
        }
    }
    fn encode(&mut self, frm: &NAFrame) -> EncoderResult<()> {
        if self.frame_no >= self.nframes {
            return Ok(());
        }
        self.frame_no += 1;
        let mut bw   = BitWriter::new(Vec::with_capacity(42), BitWriterMode::LE);

        let is_intra = match frm.get_buffer() {
                NABufferType::Video(ref buf) => {
                    self.encode_frame(&mut bw, buf)?
                },
                NABufferType::None => {
                    self.encode_skip(&mut bw)?;
                    false
                },
                _ => return Err(EncoderError::InvalidParameters),
            };
        let dbuf = bw.end();
        self.tot_size += dbuf.len();
        self.rc.update_size(dbuf.len());
        self.pkt = Some(NAPacket::new(self.stream.clone().unwrap(), frm.ts, is_intra, dbuf));

        std::mem::swap(&mut self.cur_frm, &mut self.last_frm);
        Ok(())
    }
    fn get_packet(&mut self) -> EncoderResult<Option<NAPacket>> {
        let mut npkt = None;
        std::mem::swap(&mut self.pkt, &mut npkt);
        Ok(npkt)
    }
    fn flush(&mut self) -> EncoderResult<()> {
        Ok(())
    }
}

impl Drop for BinkEncoder {
    fn drop(&mut self) {
        if self.print_stats {
            println!("encoded {} frame(s)", self.frame_no);
            println!("block statistics:");
            for (name, &count) in BLOCK_MODE_NAMES.iter().zip(self.blk_stats.iter()) {
                if count != 0 {
                    println!("    {:8}: {:8}", name, count);
                }
            }
            if self.blk_stats[usize::from(BlockMode::Intra)] != 0 {
                print!("intra block quants:");
                for &count in self.iq_stats.iter() {
                    print!(" {}", count);
                }
                println!();
            }
            if self.blk_stats[usize::from(BlockMode::Inter)] != 0 {
                print!("inter block quants:");
                for &count in self.pq_stats.iter() {
                    print!(" {}", count);
                }
                println!();
            }
            if self.frame_no > 0 {
                println!("average frame size {} byte(s)", self.tot_size / (self.frame_no as usize));
                if let Some(ref stream) = self.stream {
                    let bitrate = (self.tot_size as u64) * 8 * u64::from(stream.tb_den) / u64::from(stream.tb_num) / self.frame_no;
                    let br_fmt = if bitrate >= 10_000_000 {
                            format!("{}mbps", bitrate / 1000000)
                        } else if bitrate >= 10_000 {
                            format!("{}kbps", bitrate / 1000)
                        } else {
                            format!("{}bps", bitrate)
                        };
                    println!("average bitrate {}", br_fmt);
                }
            }
        }
    }
}

const ENCODER_OPTS: &[NAOptionDefinition] = &[
    NAOptionDefinition {
        name: "nframes", description: "duration in frames",
        opt_type: NAOptionDefinitionType::Int(Some(0), None) },
    NAOptionDefinition {
        name: "version", description: "codec version",
        opt_type: NAOptionDefinitionType::String(Some(&["b", "f", "g", "h", "i", "k"])) },
    NAOptionDefinition {
        name: "scale_mode", description: "output scaling mode",
        opt_type: NAOptionDefinitionType::String(Some(&["none", "height2x", "height_il",
                    "width2x", "width2x_il", "scale2x"])) },
    NAOptionDefinition {
        name: "forbidden", description: "block coding modes to omit (e.g. inter+residue+run)",
        opt_type: NAOptionDefinitionType::String(None) },
    NAOptionDefinition {
        name: "print_stats", description: "print internal encoding statistics at the end",
        opt_type: NAOptionDefinitionType::Bool },
];

impl NAOptionHandler for BinkEncoder {
    fn get_supported_options(&self) -> &[NAOptionDefinition] { ENCODER_OPTS }
    fn set_options(&mut self, options: &[NAOption]) {
        for option in options.iter() {
            for opt_def in ENCODER_OPTS.iter() {
                if opt_def.check(option).is_ok() {
                    match option.name {
                        "version" => {
                            if let NAValue::String(ref strval) = option.value {
                                match strval.as_str() {
                                    "b" => self.version = 'b',
                                    _ => {
                                        println!("versions beside 'b' are not supported");
                                    },
                                };
                            }
                        },
                        "scale_mode" => {
                            if let NAValue::String(ref strval) = option.value {
                                match strval.as_str() {
                                    "none" => self.scale_mode = DoublingMode::None,
                                    "height2x" => self.scale_mode = DoublingMode::Height2X,
                                    "height_il" => self.scale_mode = DoublingMode::HeightIlace,
                                    "scale2x" => self.scale_mode = DoublingMode::Scale2X,
                                    "width2x" => self.scale_mode = DoublingMode::Width2X,
                                    "width2x_il" => self.scale_mode = DoublingMode::Width2XIlace,
                                    _ => {},
                                };
                            }
                        },
                        "nframes" => {
                            if let NAValue::Int(ival) = option.value {
                                self.nframes = ival as u64;
                            }
                        },
                        "forbidden" => {
                            if let NAValue::String(ref strval) = option.value {
                                for el in self.forbidden.iter_mut() {
                                    *el = false;
                                }
                                for name in strval.split('+') {
                                    if let Ok(bmode) = name.parse::<BlockMode>() {
                                        self.forbidden[usize::from(bmode)] = true;
                                    }
                                }
                            }
                        },
                        "print_stats" => {
                            if let NAValue::Bool(bval) = option.value {
                                self.print_stats = bval;
                            }
                        },
                        _ => {},
                    };
                }
            }
        }
    }
    fn query_option_value(&self, name: &str) -> Option<NAValue> {
        match name {
            "version" => Some(NAValue::String(self.version.to_string())),
            "scale_mode" => Some(NAValue::String(self.scale_mode.to_string())),
            "nframes" => Some(NAValue::Int(self.nframes as i64)),
            "forbidden" => {
                let mut result = String::new();
                for (name, &flag) in BLOCK_MODE_NAMES.iter().zip(self.forbidden.iter()) {
                    if flag {
                        if !result.is_empty() {
                            result.push('+');
                        }
                        result += name;
                    }
                }
                Some(NAValue::String(result))
            },
            "print_stats" => Some(NAValue::Bool(self.print_stats)),
            _ => None,
        }
    }
}

pub fn get_encoder() -> Box<dyn NAEncoder + Send> {
    Box::new(BinkEncoder::new())
}

#[cfg(test)]
mod test {
    use nihav_core::codecs::*;
    use nihav_core::demuxers::*;
    use nihav_core::muxers::*;
    use crate::*;
    use nihav_codec_support::test::enc_video::*;
    use nihav_commonfmt::*;

    fn test_bink_encoder(out_name: &'static str, enc_options: &[NAOption], br: u32, quality: u8, hash: &[u32; 4]) {
        let mut dmx_reg = RegisteredDemuxers::new();
        generic_register_all_demuxers(&mut dmx_reg);
        let mut dec_reg = RegisteredDecoders::new();
        generic_register_all_decoders(&mut dec_reg);
        let mut mux_reg = RegisteredMuxers::new();
        rad_register_all_muxers(&mut mux_reg);
        let mut enc_reg = RegisteredEncoders::new();
        rad_register_all_encoders(&mut enc_reg);

        // sample from private collection
        let dec_config = DecoderTestParams {
                demuxer:        "yuv4mpeg",
                in_name:        "assets/day3b.y4m",
                stream_type:    StreamType::Video,
                limit:          None,
                dmx_reg, dec_reg,
            };
        let enc_config = EncoderTestParams {
                muxer:          "bink",
                enc_name:       "bink-video",
                out_name,
                mux_reg, enc_reg,
            };
        let dst_vinfo = NAVideoInfo {
                width:   0,
                height:  0,
                format:  YUV420_FORMAT,
                flipped: false,
                bits:    8,
            };
        let enc_params = EncodeParameters {
                format:  NACodecTypeInfo::Video(dst_vinfo),
                quality,
                bitrate: br * 1000,
                tb_num:  0,
                tb_den:  0,
                flags:   0,
            };
        let _ = hash;
        //test_encoding_to_file(&dec_config, &enc_config, enc_params, enc_options);
        test_encoding_md5(&dec_config, &enc_config, enc_params, enc_options, hash);
    }
    #[test]
    fn test_binkb_quality() {
        let enc_options = &[
                NAOption { name: "nframes", value: NAValue::Int(7) },
                //NAOption { name: "print_stats", value: NAValue::Bool(true) },
            ];
        test_bink_encoder("bink-b-q50.bik", enc_options, 0, 50, &[0xd83936aa, 0xec3f55d4, 0x25e5c1fb, 0x0f3454ce]);
        let enc_options = &[
                NAOption { name: "nframes", value: NAValue::Int(7) },
                //NAOption { name: "print_stats", value: NAValue::Bool(true) },
            ];
        test_bink_encoder("bink-b-q75.bik", enc_options, 0, 75, &[0x45ccd3d4, 0xf09bd106, 0xc88751db, 0xca5294d7]);
        let enc_options = &[
                NAOption { name: "nframes", value: NAValue::Int(7) },
                //NAOption { name: "print_stats", value: NAValue::Bool(true) },
            ];
        test_bink_encoder("bink-b-q99.bik", enc_options, 0, 99, &[0xb516554e, 0xca025167, 0xd6c3dc06, 0x00e6ba25]);
    }
    #[test]
    fn test_binkb_features() {
        let enc_options = &[
                NAOption { name: "nframes", value: NAValue::Int(7) },
                NAOption { name: "forbidden", value: NAValue::String("intra+inter".to_string()) },
                //NAOption { name: "print_stats", value: NAValue::Bool(true) },
            ];
        test_bink_encoder("bink-b-nodct.bik", enc_options, 0, 0, &[0x5e098760, 0x31c8982a, 0x90ce8441, 0x859d3cc6]);
        let enc_options = &[
                NAOption { name: "nframes", value: NAValue::Int(7) },
                NAOption { name: "forbidden", value: NAValue::String("skip+fill+run+pattern+residue+raw".to_string()) },
                //NAOption { name: "print_stats", value: NAValue::Bool(true) },
            ];
        test_bink_encoder("bink-b-dct.bik", enc_options, 0, 0, &[0xed2fc7d2, 0x8a7a05ef, 0xd0b4ae2c, 0x622a4ef0]);
    }
}
