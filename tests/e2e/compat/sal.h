#pragma once
#ifdef _WIN32
#error "tests/e2e/compat/sal.h must never be included on _WIN32"
#endif

#ifndef _In_
#define _In_
#endif
#ifndef _In_opt_
#define _In_opt_
#endif
#ifndef _In_z_
#define _In_z_
#endif
#ifndef _In_reads_
#define _In_reads_(n)
#endif
#ifndef _In_reads_opt_
#define _In_reads_opt_(n)
#endif
#ifndef _In_reads_bytes_
#define _In_reads_bytes_(n)
#endif
#ifndef _In_reads_bytes_opt_
#define _In_reads_bytes_opt_(n)
#endif
#ifndef _Out_
#define _Out_
#endif
#ifndef _Out_opt_
#define _Out_opt_
#endif
#ifndef _Out_writes_
#define _Out_writes_(n)
#endif
#ifndef _Out_writes_opt_
#define _Out_writes_opt_(n)
#endif
#ifndef _Out_writes_bytes_
#define _Out_writes_bytes_(n)
#endif
#ifndef _Out_writes_to_
#define _Out_writes_to_(size, count)
#endif
#ifndef _Out_writes_to_opt_
#define _Out_writes_to_opt_(size, count)
#endif
#ifndef _Out_writes_bytes_to_
#define _Out_writes_bytes_to_(size, count)
#endif
#ifndef _Out_writes_bytes_to_opt_
#define _Out_writes_bytes_to_opt_(size, count)
#endif
#ifndef _Inout_
#define _Inout_
#endif
#ifndef _Inout_opt_
#define _Inout_opt_
#endif
#ifndef _Inout_updates_
#define _Inout_updates_(n)
#endif
#ifndef _Inout_updates_bytes_
#define _Inout_updates_bytes_(n)
#endif
#ifndef _Ret_range_
#define _Ret_range_(lb, ub)
#endif
#ifndef _Ret_maybenull_
#define _Ret_maybenull_
#endif
#ifndef _Success_
#define _Success_(expr)
#endif
#ifndef _Use_decl_annotations_
#define _Use_decl_annotations_
#endif
#ifndef _Must_inspect_result_
#define _Must_inspect_result_
#endif
