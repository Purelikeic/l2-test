"""Evidence labels for the implemented limited DUT profiles, not global acceptance."""
SCOPES = {
    "deterministic-read": "limited serialized read/upgrade DUT profile with separated completion and Retry/P-Credit",
    "random-read": "limited serialized four-identity, two-line random read DUT profile",
    "lower-snoop": "limited stable-line dirty Shared Snoop/Home-memory commitment and clean UniqueFwd/DCT DUT profile",
    "victim": "limited default-geometry serialized RRIP victim/dirty-client recall, both clean replacement branches and actual Home MemoryAgent copyback commitment; not full internal-state/global acceptance",
}
