// rex/hook.h for SDK 0.7.3 - the few SDK 0.10 hook macros the w13r renderer uses (decision 0008).
#pragma once
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/logging.h>
#include "../w13r_compat.h"
#define REX_FUNC(x) PPC_FUNC(x)
#define REX_EXTERN(x) PPC_EXTERN_FUNC(x)
#define REX_HOOK(subroutine, function) \
  extern "C" REX_FUNC(subroutine) { rex::HostToGuestFunction<function>(ctx, base); }
#define REX_HOOK_RAW(name) extern "C" REX_FUNC(name)
namespace rex::ppc { using rex::HostToGuestFunction; }
