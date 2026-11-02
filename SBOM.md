# Source SBOM

Source repository inventory only; completeness is UNKNOWN. Syft inventories tracked source/package manifests. Gitlinks and declared vendored/model evidence are inspected at immutable pins. CMake declarations are conditional requirements, not resolved installations. This is NOT proof of operating-system packages, static-link closure, build-time resolution, or the contents of release binaries.

Generation: **success**

## Source identity

wave-owl/ale-core `8692cdf55fc716acef60df299768f3ca0daf5556`

- sbom:role: source-root
- sbom:git-commit: 8692cdf55fc716acef60df299768f3ca0daf5556
- sbom:git-tree: 84d89ed3b1ef37bf5ec017116c12280d5edd0f88
- sbom:source-repository: https://github.com/wave-owl/ale-core
- sbom:release-tags: ["v0.1.0"]
- sbom:license-evidence: [{"path": "LICENSE", "sha256": "3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986"}]
- sbom:license-resolution: unresolved; manual review required

## Generation errors

None.

## Components and unresolved licenses

| Component | Version | Role | License |
| --- | --- | --- | --- |
| wave-owl/ale-core | 8692cdf55fc716acef60df299768f3ca0daf5556 | source-root | unresolved; manual review required |
| src/ale_rx.c | 3b570602a14161188f8c85eede62ab2a70e7ad66 | declared-vendored-source | unresolved; manual review required |
| src/ale_tables.c | 3b570602a14161188f8c85eede62ab2a70e7ad66 | declared-vendored-source | unresolved; manual review required |
| src/ale_tx.c | 1f0fb89a4b3c9cd6b6cbb0a41e7ae8f817377b48 | declared-vendored-source | unresolved; manual review required |
| actions/checkout | v4 | Syft source/package discovery | unresolved |
| tracked-source/.github/workflows/ci.yml | unresolved | Syft source/package discovery | unresolved |

License evidence paths/hashes, source manifests, model hashes and conditional
CMake declarations are recorded in SBOM.cdx.json. Declared upstream pins do not
prove vendored equivalence. No dependency installation or build was performed.
