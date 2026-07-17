// Copyright (C) 2026, All rights reserved by Niobium Microsystems.
// The contents of this file and all related materials provided herein (the
// "Product") may not be used except pursuant to a separate written
// agreement signed by a duly authorized officer of Niobium Microsystems,
// Inc. (a "License Agreement").
// Without limiting the foregoing, you may not, at any time or for any
// reason, directly or indirectly, in whole or in part: (i) copy, modify,
// or create derivative works of the Product; (ii) rent, lease, lend, sell,
// sublicense, assign, distribute, publish, transfer, or otherwise make
// available the Product; (iii) reverse engineer, disassemble, decompile,
// decode, or adapt the Product; or (iv) remove any proprietary notices
// from the Product.
//
// Graph capture / execution shims (CUDA-shape names). Each entry validates
// its arguments and forwards to the core graph module; output handles are
// zeroed on entry-validation failure.

#include "core/graph.hpp"

#include "common/errors.hpp"

#include <haze/haze.h>
#include <haze/haze_types.h>

extern "C" hazeError_t hazeStreamBeginCapture(hazeStream_t stream) noexcept {
    return set_internal_result(haze::graph_begin_capture(stream));
}

extern "C" hazeError_t hazeStreamEndCapture(hazeStream_t stream, hazeGraph_t *graph) noexcept {
    if (graph == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    *graph = nullptr;
    auto result = haze::graph_end_capture(stream);
    if (!result)
        return set_error(haze::to_public_error(result.error()));
    *graph = *result;
    return set_error(HAZE_SUCCESS);
}

extern "C" hazeError_t hazeGraphInstantiate(hazeGraphExec_t *exec, hazeGraph_t graph) noexcept {
    if (exec == nullptr)
        return set_error(HAZE_ERROR_INVALID_VALUE);
    *exec = nullptr;
    auto result = haze::graph_instantiate(graph);
    if (!result)
        return set_error(haze::to_public_error(result.error()));
    *exec = *result;
    return set_error(HAZE_SUCCESS);
}

extern "C" hazeError_t hazeGraphLaunch(hazeGraphExec_t exec, hazeStream_t stream) noexcept {
    return set_internal_result(haze::graph_launch(exec, stream));
}

extern "C" hazeError_t hazeGraphExecUpdate(hazeGraphExec_t exec, hazeGraph_t graph) noexcept {
    return set_internal_result(haze::graph_exec_update(exec, graph));
}

extern "C" hazeError_t hazeGraphExecDestroy(hazeGraphExec_t exec) noexcept {
    return set_internal_result(haze::graph_exec_destroy(exec));
}

extern "C" hazeError_t hazeGraphDestroy(hazeGraph_t graph) noexcept {
    return set_internal_result(haze::graph_destroy(graph));
}
