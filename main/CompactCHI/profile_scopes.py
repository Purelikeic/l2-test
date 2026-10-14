"""Evidence labels for the implemented limited DUT profiles, not global acceptance."""
SCOPES = {
    "stress": "sustained configurable endpoint traffic with per-node multiple outstanding, stream/same-set/mixed boundaries and final drain; current supported profile only",
    "deterministic-read": "limited serialized read/upgrade DUT profile with separated completion and Retry/P-Credit",
    "random-read": "limited serialized four-identity, two-line random read DUT profile",
    "action-replay": "serialized complete accepted action intent replay with original seed/configuration; no exact-cycle/global acceptance",
    "random-actions": "bounded seeded serialized legal coherent/transient/MMIO action segments; under-capacity working set, no arbitrary overlap/global acceptance",
    "lower-snoop": "limited stable-line dirty Shared Snoop/Home-memory commitment and clean UniqueFwd/DCT DUT profile",
    "node-actions": "limited configured coherent/transient/MMIO node actions with MakeUnique full overwrite, Evict/WriteBack, CMO and noncoherent readback; not full global acceptance",
    "configured": "configured protocol-neutral node action scheduler using original INI seed/ARI/stream policy; not full global acceptance",
    "victim": "limited default-geometry serialized RRIP victim/dirty-client recall, both clean replacement branches and actual Home MemoryAgent copyback commitment; not full internal-state/global acceptance",
}
