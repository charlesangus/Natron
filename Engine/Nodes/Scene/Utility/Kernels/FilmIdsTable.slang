module FilmIdsTable;

// The planes of Kernels/FilmPick.slang's picks that the ids read.
public static const uint kPickHitPlane = 1;
public static const uint kPickDepthPlane = 2;
public static const uint kPickPrimIdPlane = 3;
public static const uint kPickInstanceIdPlane = 4;

// kIdCapacity and kIdSlots in UtilityRenderBuffer.cpp mirror these. The id table is one buffer of words: the
// entries taken, the pairs that found no room, and the pixels whose pair it does not hold; then kIdSlots slots
// of two words, a pair's tag and its entry plus one, open-addressed from idSlot(); then kIdCapacity entries of
// two words, a prim id and an instance id. A tag is never 0, the word of a free slot.
public static const uint kIdCapacity = 65536;
public static const uint kIdSlots = 2 * kIdCapacity;
public static const uint kIdMaxProbes = 64;
public static const uint kIdUsedWord = 0;
public static const uint kIdOverflowWord = 1;
public static const uint kIdCollisionsWord = 2;
public static const uint kIdSlotsWord = 4;
public static const uint kIdEntriesWord = kIdSlotsWord + 2 * kIdSlots;

public uint idTag(uint prim, uint instance)
{
    uint h = (prim * 0x9E3779B1u) ^ (instance + 0x7F4A7C15u + (prim << 6) + (prim >> 2));

    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;

    return h | 1u;
}

public uint idSlot(uint tag)
{
    return (tag >> 1) & (kIdSlots - 1u);
}

// The entry plus one of the slot holding tag, 0 when none does.
public uint findIdEntry(StructuredBuffer<uint> table, uint tag)
{
    const uint start = idSlot(tag);

    for (uint probe = 0; probe < kIdMaxProbes; ++probe) {
        const uint slot = (start + probe) & (kIdSlots - 1u);
        const uint word = table[kIdSlotsWord + 2u * slot];

        if (word == tag)
            return table[kIdSlotsWord + 2u * slot + 1u];
        if (word == 0u)
            return 0u;
    }

    return 0u;
}

public uint findIdEntry(RWStructuredBuffer<uint> table, uint tag)
{
    const uint start = idSlot(tag);

    for (uint probe = 0; probe < kIdMaxProbes; ++probe) {
        const uint slot = (start + probe) & (kIdSlots - 1u);
        const uint word = table[kIdSlotsWord + 2u * slot];

        if (word == tag)
            return table[kIdSlotsWord + 2u * slot + 1u];
        if (word == 0u)
            return 0u;
    }

    return 0u;
}
