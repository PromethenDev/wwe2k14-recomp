// Global type exports as in ReXGlue SDK 0.10 rex/types.h (Copyright (c) 2026 Tom Clay, BSD 3-Clause).


//=============================================================================
// Global Namespace Exports
//=============================================================================

using u8 = rex::u8;
using i8 = rex::i8;
using u16 = rex::u16;
using i16 = rex::i16;
using u32 = rex::u32;
using i32 = rex::i32;
using u64 = rex::u64;
using i64 = rex::i64;
using f32 = rex::f32;
using f64 = rex::f64;

using be_u16 = rex::be_u16;
using be_u32 = rex::be_u32;
using be_u64 = rex::be_u64;
using be_i16 = rex::be_i16;
using be_i32 = rex::be_i32;
using be_i64 = rex::be_i64;
using be_f32 = rex::be_f32;
using be_f64 = rex::be_f64;

using mapped_void = rex::MappedPtr<void>;
using mapped_u8 = rex::MappedPtr<uint8_t>;
using mapped_u16 = rex::MappedPtr<rex::be_u16>;
using mapped_u32 = rex::MappedPtr<rex::be_u32>;
using mapped_u64 = rex::MappedPtr<rex::be_u64>;
using mapped_f32 = rex::MappedPtr<rex::be_f32>;
using mapped_f64 = rex::MappedPtr<rex::be_f64>;
using mapped_string = rex::MappedPtr<char>;
using mapped_wstring = rex::MappedPtr<char16_t>;

