/*
    SPDX-FileCopyrightText: 2026 Majd Abu Diab
    SPDX-License-Identifier: CC0-1.0
*/
#pragma once

#cmakedefine01 HAVE_SYS_PRCTL_H
#cmakedefine01 HAVE_SYS_PROCCTL_H
#cmakedefine01 HAVE_PR_SET_DUMPABLE
#cmakedefine01 HAVE_PROC_TRACE_CTL

#define STOCK_POLKIT_AGENT "${STOCK_POLKIT_AGENT}"
#define KENNING_SYSTEM_CONFIG "${KENNING_SYSTEM_CONFIG}"
