import FilmIdsTable;

// KernelFilmIdsParams in UtilityRenderBuffer.cpp mirrors this. Over a window of a frame's picks, as
// FilmPick.slang lays them out, it gathers the distinct (prim id, instance id) pairs of the pixels that hit a
// prim into the id table FilmIdsTable.slang describes.
struct FilmIdsParams
{
    // The window's size; width * height is the length of each plane of the picks.
    uint width;
    uint height;
    // 0 gathers the pairs; 1, a dispatch of its own after the gathering, counts the pixels whose pair the table
    // does not hold, as a pair whose tag another took leaves it.
    uint mode;
    uint padding0;
};

[[vk::binding(0, 0)]] StructuredBuffer<uint> picks;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> table;
[[vk::push_constant]] ConstantBuffer<FilmIdsParams> params;

[shader("compute")]
[numthreads(8, 8, 1)]
void utilityFilmIds(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= params.width || id.y >= params.height)
        return;

    const uint n = params.width * params.height;
    const uint at = id.y * params.width + id.x;

    if (picks[kPickHitPlane * n + at] == 0u)
        return;

    const uint prim = picks[kPickPrimIdPlane * n + at];
    const uint instance = picks[kPickInstanceIdPlane * n + at];

    if (int(prim) < 0)
        return;

    const uint tag = idTag(prim, instance);
    const uint start = idSlot(tag);

    if (params.mode == 0u) {
        for (uint probe = 0; probe < kIdMaxProbes; ++probe) {
            const uint slot = (start + probe) & (kIdSlots - 1u);
            uint previous = 0u;

            InterlockedCompareExchange(table[kIdSlotsWord + 2u * slot], 0u, tag, previous);
            if (previous == 0u) {
                uint entry = 0u;

                InterlockedAdd(table[kIdUsedWord], 1u, entry);
                if (entry >= kIdCapacity) {
                    InterlockedAdd(table[kIdOverflowWord], 1u);
                    return;
                }
                table[kIdEntriesWord + 2u * entry] = prim;
                table[kIdEntriesWord + 2u * entry + 1u] = instance;
                table[kIdSlotsWord + 2u * slot + 1u] = entry + 1u;
                return;
            }
            if (previous == tag)
                return;
        }
        InterlockedAdd(table[kIdOverflowWord], 1u);
        return;
    }

    const uint entry = findIdEntry(table, tag);

    if ((entry == 0u) || (table[kIdEntriesWord + 2u * (entry - 1u)] != prim) || (table[kIdEntriesWord + 2u * (entry - 1u) + 1u] != instance))
        InterlockedAdd(table[kIdCollisionsWord], 1u);
}
