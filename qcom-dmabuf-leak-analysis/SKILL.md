---
name: qcom-dmabuf-leak-analysis
description: Diagnose Qualcomm Android DMA-BUF growth and native pipeline memory leaks by correlating dmabuf_dump/process totals, /proc fd and inode data, allocation/free hooks, symbolized stacks, HcMat ownership, FastRPC/SNPE runtime buffers, and pipeline teardown. Use when a process grows by a fixed DMA size per loop, alloc/free hooks miss an apparent leak, releaseHandler or dlclose does not reduce DMA usage, or raii=false HcMat/device memory ownership is suspected.
---

# Qualcomm DMA-BUF Leak Analysis

Use this workflow to distinguish a real per-iteration leak from allocator/cache/runtime residency and to prove whether pipeline teardown releases the underlying DMA-BUF.

## 1. Establish the symptom

Collect, for the same PID and same run:

- DMA/process totals before the loop, after each iteration, after `releaseHandler`, and after `dlclose`.
- `/proc/<pid>/fd` links, fdinfo sizes, fd numbers, and inode numbers at the same points.
- Allocation-hook output with allocation and free events, sizes, fds, inodes, and stacks.
- The exact source/build IDs for the tested native library and executable.

Do not equate `PROCESS TOTAL` with leaked allocations. It includes buffers held by FastRPC, SNPE, OpenCL/Vulkan registries, memory pools, and driver/runtime caches.

## 2. Correlate by inode, not fd

FDs are reusable. Pair allocation and free events by DMA-BUF inode; use fd only as a diagnostic attribute. A robust hook should:

1. Record the returned allocation fd and resolve its inode after the ioctl succeeds.
2. Record allocation size and stack.
3. Resolve the inode again on free/close paths when possible.
4. Report an allocation as unreleased only when its inode has no matching free.

When instrumenting loop behavior, snapshot the inode set before an iteration and compare it with the set after teardown. Avoid a broad size-only rule: FastRPC commonly creates persistent 4 KB and 256 KB buffers that can cause false iteration-timeout reports.

## 3. Read the stack before changing teardown

Classify each residual allocation by the first meaningful owner frame:

- `HcMat::HcMat -> devmem_alloc_internal`: inspect HcMat ownership and destructor semantics.
- `rpcmem_alloc_internal -> fastrpc_config_sharedbufs`, `persist_buf_init`, or `listener_start_thread`: runtime-owned FastRPC/SNPE buffer; do not call it a pipeline leak without a separate lifecycle proof.
- External image fd/buffer paths: inspect the external buffer cache and producer/consumer ownership.

Use the exact library BuildId and matching unstripped symbols with `llvm-addr2line`. Never symbolize against a different build.

## 4. Check HcMat ownership semantics

In the HAIO HcMat implementation:

- `raii=true` creates a shared-data deleter that calls `dev_mem::release_ptr()` and removes registered CL/Vulkan memory first.
- `raii=false` leaves the backing device allocation to the owner; the default HcMat destructor does not release it.
- `HcOpr::free_cl_mem()` unregisters/releases the OpenCL object/registry entry; it is not a replacement for `dev_mem::release_ptr()`.
- `CVMAT()`/`to_cv()` may require `m_raii == false`, so do not blindly change such buffers to RAII ownership.

For pipeline-owned non-RAII HcMats, teardown must follow this order:

1. Stop/abort new work.
2. Wait for every async task and command queue.
3. Clear recorders/caches that retain pointers.
4. Unregister model ION/external bindings.
5. Unregister OpenCL/Vulkan memory.
6. Call `dev_mem::release_ptr(mat.data)` exactly once.
7. Null `data` and `data_ori` to prevent accidental reuse.

Use an idempotent helper guarded by `mat.data != nullptr`. Release partial-construction allocations too; constructors that throw do not run the class destructor.

## 5. Use size arithmetic as a confirmation, not proof

If the observed growth is 304 KB, sum candidate allocations from the same pipeline lifetime. In the investigated TMC pipeline, the six non-RAII members were:

```text
32 KB + 32 KB + 4 KB + 52 KB + 52 KB + 132 KB = 304 KB
```

The arithmetic becomes strong evidence only when the allocation stacks resolve to the corresponding constructor lines and the same inodes remain live after pipeline destruction.

## 6. Verify the fix

Run at least two create/process/release cycles and check all of the following:

- The per-iteration inode wait completes without a long timeout.
- The six candidate allocation inodes receive free events after pipeline destruction.
- `PROCESS TOTAL` is stable across iterations.
- The hook reports no unreleased candidate allocations; any remaining allocation has a runtime stack and stable inode.
- `dlclose` is not used as a substitute for object ownership cleanup.

Expected residuals such as a single stable 1 MB FastRPC/SNPE buffer should be reported separately from pipeline-owned DMA. Do not require total DMA to become zero unless the runtime lifecycle explicitly promises that behavior.

## Common false conclusions

- “`free_cl_mem` succeeded, so device memory is freed.” It may only remove CL registration.
- “The fd changed, so it is a new leak.” Compare inode; fd reuse is normal.
- “The hook missed it.” First check whether the hook supports both DMA heap and ION allocation ioctls and whether matching is inode-based.
- “A 4 KB residual after release proves the pipeline leaked.” Check FastRPC stack frames first.
- “Increasing the wait timeout fixes the leak.” A timeout only changes observation; it does not release ownership.

## Evidence format

Report conclusions using three labels:

- **Evidence:** exact file/line, log line, inode, size, stack, or free event.
- **Inference:** the ownership/lifecycle conclusion derived from those facts.
- **Unknown:** behavior requiring a device run, matching symbols, or vendor runtime confirmation.

Always include changed files, verification commands/results, remaining runtime buffers, and the distinction between process-total stability and complete process teardown.
