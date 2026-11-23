// Copyright (C) 2026 WWE 2K14 Native contributors.
// Distributed under the GNU Affero General Public License version 3 only.
// The Bink/Bink 2 decoder is NihAV; see NOTICE.md and vendor/nihav/COPYING.
mod encoder;
mod tables;

use encoder::{BinkIEncoder, YuvFrame};
use nihav_core::codecs::{FrameType, NADecoderSupport, RegisteredDecoders};
use nihav_core::demuxers::{create_demuxer, DemuxerError, RegisteredDemuxers};
use nihav_core::frame::NAFrameRef;
use std::ffi::{OsStr, OsString};
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};

fn usage() -> &'static str {
    "usage: wwe2k14-bink-convert IN.bik2 OUT.bik2 [--progress]"
}

fn le_u32(bytes: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
}

fn input_header(input: &Path) -> Result<([u8; 44], u32), String> {
    let mut file = File::open(input).map_err(|e| format!("Could not open the input movie: {e}"))?;
    let mut header = [0u8; 44];
    file.read_exact(&mut header).map_err(|e| format!("The input movie header is incomplete: {e}"))?;
    if &header[..4] != b"KB2d" && &header[..4] != b"KB2i" {
        return Err("The input must be a WWE '13 (KB2d) or WWE 2K15 (KB2i) Bink 2 movie.".into());
    }
    let frames = le_u32(&header, 8);
    let width = le_u32(&header, 20);
    let height = le_u32(&header, 24);
    let tb_den = le_u32(&header, 28);
    let tb_num = le_u32(&header, 32);
    if frames == 0 || width == 0 || height == 0 || width % 16 != 0 || height % 16 != 0 || tb_den == 0 || tb_num == 0 {
        return Err("The input movie has unsupported dimensions, frame rate, or frame count.".into());
    }
    Ok((header, frames))
}

fn extract_yuv420(frame: &NAFrameRef, width: usize, height: usize) -> Result<YuvFrame, String> {
    let buffer = frame.get_buffer().get_vbuf()
        .ok_or_else(|| "The decoder returned a frame format other than 8-bit YUV video.".to_string())?;
    let (yw, yh) = buffer.get_dimensions(0);
    let (cw, ch) = buffer.get_dimensions(1);
    if yw != width || yh != height || cw != width / 2 || ch != height / 2 ||
       buffer.get_dimensions(2) != (cw, ch) {
        return Err("The decoder returned a frame layout other than YUV 4:2:0.".into());
    }
    let data = buffer.get_data();
    let flipped = buffer.get_info().is_flipped();
    let mut y = vec![0u8; width * height];
    let mut u = vec![0u8; cw * ch];
    let mut v = vec![0u8; cw * ch];
    let y_stride = buffer.get_stride(0);
    let u_stride = buffer.get_stride(1);
    let v_stride = buffer.get_stride(2);
    let y_base = buffer.get_offset(0);
    let u_base = buffer.get_offset(1);
    let v_base = buffer.get_offset(2);
    if y_stride < width || u_stride < cw || v_stride < cw {
        return Err("The decoder returned an invalid plane stride.".into());
    }
    for row in 0..height {
        let source_row = if flipped { height - 1 - row } else { row };
        let start = y_base + source_row * y_stride;
        let source = data.get(start..start + width).ok_or_else(|| "The decoded luma plane is truncated.".to_string())?;
        y[row * width..(row + 1) * width].copy_from_slice(source);
    }
    for row in 0..ch {
        let source_row = if flipped { ch - 1 - row } else { row };
        let u_start = u_base + source_row * u_stride;
        let v_start = v_base + source_row * v_stride;
        let u_source = data.get(u_start..u_start + cw).ok_or_else(|| "The decoded U plane is truncated.".to_string())?;
        let v_source = data.get(v_start..v_start + cw).ok_or_else(|| "The decoded V plane is truncated.".to_string())?;
        u[row * cw..(row + 1) * cw].copy_from_slice(u_source);
        v[row * cw..(row + 1) * cw].copy_from_slice(v_source);
    }
    Ok(YuvFrame { y, u, v })
}

fn thread_count() -> usize {
    let requested = std::env::var("WWE_BINK_THREADS").ok().and_then(|v| v.parse::<usize>().ok());
    requested.unwrap_or_else(|| std::thread::available_parallelism().map(usize::from).unwrap_or(1)).clamp(1, 64)
}

fn report_progress(done: u32, total: u32) {
    let percent = done.saturating_mul(100) / total.max(1);
    println!("PROGRESS\t{done}\t{total}\t{percent}%");
    let _ = io::stdout().flush();
}

fn run(input: &Path, output: &Path, with_progress: bool) -> Result<(), String> {
    if input == output { return Err("Input and output paths must be different.".into()); }
    let (header, expected_frames) = input_header(input)?;
    let width = le_u32(&header, 20) as usize;
    let height = le_u32(&header, 24) as usize;
    let tb_den = le_u32(&header, 28);
    let tb_num = le_u32(&header, 32);

    let mut demuxers = RegisteredDemuxers::new();
    nihav_rad::rad_register_all_demuxers(&mut demuxers);
    let demuxer_factory = demuxers.find_demuxer("bink")
        .ok_or_else(|| "The Bink demuxer is not available in this build.".to_string())?;
    let mut input_file = File::open(input).map_err(|e| format!("Could not open the input movie: {e}"))?;
    let mut reader = nihav_core::io::byteio::FileReader::new_read(&mut input_file);
    let mut demuxer = create_demuxer(demuxer_factory, &mut reader)
        .map_err(|e| format!("The input movie could not be read: {e:?}"))?;

    let mut decoders = RegisteredDecoders::new();
    nihav_rad::rad_register_all_decoders(&mut decoders);
    let (video_stream, mut decoder, mut support) = (0..demuxer.get_num_streams())
        .filter_map(|i| demuxer.get_stream(i).map(|stream| (i, stream)))
        .find_map(|(i, stream)| {
            let info = stream.get_info();
            if !info.is_video() { return None; }
            let factory = decoders.find_decoder(info.get_name())?;
            let mut decoder = factory();
            let mut support = Box::new(NADecoderSupport::new());
            decoder.init(&mut support, info).ok()?;
            Some((i, decoder, support))
        })
        .ok_or_else(|| "The input movie has no supported video stream.".to_string())?;

    let workers = thread_count();
    let mut encoder = BinkIEncoder::new(width, height);
    let mut decoded = 0u32;
    loop {
        let packet = match demuxer.get_frame() {
            Ok(packet) => packet,
            Err(DemuxerError::EOF) => break,
            Err(e) => return Err(format!("The input movie ended with a read error: {e:?}")),
        };
        if packet.get_stream().get_id() as usize != video_stream { continue; }
        let frame = decoder.decode(&mut support, &packet)
            .map_err(|e| format!("A video frame could not be decoded: {e:?}"))?;
        if frame.get_frame_type() == FrameType::Skip {
            return Err("The decoder skipped a source frame; no incomplete output was written.".into());
        }
        encoder.push(extract_yuv420(&frame, width, height)?, workers)?;
        decoded += 1;
        if with_progress { report_progress(decoded, expected_frames); }
    }
    if decoded != expected_frames {
        return Err(format!("The movie declared {expected_frames} frames, but only {decoded} were decoded."));
    }
    let (output_bytes, largest, psnr) = encoder.finish(tb_den, tb_num)?;
    let mut part_os: OsString = output.as_os_str().to_os_string();
    part_os.push(".part");
    let part = PathBuf::from(part_os);
    let write_result = (|| -> io::Result<()> {
        let mut file = OpenOptions::new().write(true).create(true).truncate(true).open(&part)?;
        file.write_all(&output_bytes)?;
        file.sync_all()?;
        drop(file);
        replace_file(&part, output)
    })();
    if let Err(e) = write_result {
        let _ = fs::remove_file(&part);
        return Err(format!("Could not save the converted movie: {e}"));
    }
    eprintln!("frames={decoded} bytes={} largest={largest} q=13 skip_avg_x10=25 luma_psnr={psnr:.2} threads={workers}", output_bytes.len());
    Ok(())
}

#[cfg(not(windows))]
fn replace_file(from: &Path, to: &Path) -> io::Result<()> { fs::rename(from, to) }

#[cfg(windows)]
fn replace_file(from: &Path, to: &Path) -> io::Result<()> {
    use std::os::windows::ffi::OsStrExt;
    #[link(name = "kernel32")]
    extern "system" { fn MoveFileExW(existing: *const u16, replacement: *const u16, flags: u32) -> i32; }
    let from: Vec<u16> = from.as_os_str().encode_wide().chain(Some(0)).collect();
    let to: Vec<u16> = to.as_os_str().encode_wide().chain(Some(0)).collect();
    const MOVEFILE_REPLACE_EXISTING: u32 = 0x1;
    const MOVEFILE_WRITE_THROUGH: u32 = 0x8;
    let status = unsafe { MoveFileExW(from.as_ptr(), to.as_ptr(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) };
    if status == 0 { Err(io::Error::last_os_error()) } else { Ok(()) }
}

fn main() {
    let mut args = std::env::args_os();
    let _program = args.next();
    let input = args.next();
    let output = args.next();
    let trailing: Vec<OsString> = args.collect();
    let (input, output) = match (input, output) {
        (Some(input), Some(output)) => (PathBuf::from(input), PathBuf::from(output)),
        _ => { eprintln!("{}", usage()); std::process::exit(2); }
    };
    let with_progress = match trailing.as_slice() {
        [] => false,
        [flag] if flag == OsStr::new("--progress") => true,
        [flag] if flag == OsStr::new("--help") || flag == OsStr::new("-h") => {
            println!("{}", usage()); return;
        }
        _ => { eprintln!("{}", usage()); std::process::exit(2); }
    };
    if let Err(message) = run(&input, &output, with_progress) {
        eprintln!("ERROR\t{message}");
        std::process::exit(1);
    }
}
