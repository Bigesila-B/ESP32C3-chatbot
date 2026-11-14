/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "port/mpconfigport_common.h"

#define MICROPY_CONFIG_ROM_LEVEL        (MICROPY_CONFIG_ROM_LEVEL_CORE_FEATURES)
#define MICROPY_ENABLE_COMPILER         (1)
#define MICROPY_ENABLE_EXTERNAL_IMPORT (1)
#define MICROPY_ENABLE_GC               (1)
#define MICROPY_ENABLE_VM_ABORT         (1)
#define MICROPY_FLOAT_IMPL              (MICROPY_FLOAT_IMPL_DOUBLE)
#define MICROPY_LONGINT_IMPL            (MICROPY_LONGINT_IMPL_MPZ)
#define MICROPY_NLR_SETJMP              (1)
#define MICROPY_PY_ATTRTUPLE            (1)
#define MICROPY_PY_BUILTINS_STR_UNICODE  (1)
#define MICROPY_PY_IO                   (1)
#define MICROPY_PY_JSON                 (1)
#define MICROPY_PY_MATH_CONSTANTS       (1)
#define MICROPY_PY_SYS_PLATFORM         "esp-claw"
#define MICROPY_STACK_CHECK             (1)
#define MICROPY_STACK_CHECK_MARGIN      (2048)

void cap_micropython_vm_poll(void);
#define MICROPY_VM_HOOK_INIT cap_micropython_vm_poll();
#define MICROPY_VM_HOOK_LOOP cap_micropython_vm_poll();
#define MICROPY_VM_HOOK_RETURN cap_micropython_vm_poll();
