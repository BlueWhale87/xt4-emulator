# Covered NOR source contract

The pure `XT4CoveredNor` helper represents a borrowed immutable byte extent,
not a complete installed-media image. A declared64MiB capacity provides a bound;
it supplies no bytes for uncovered addresses. It has no file, timer or device
backend.

Its virtual address policy is strict subtraction from `f0000000` followed by
capacity and complete-coverage checks. An out-of-window address does not wrap.
Every requested byte must lie within the supplied extent before any output is
copied. Failure leaves the output buffer unchanged. This policy is a virtual
integration choice motivated by the firmware's address mapping; sequential NOR
rollover does not establish masking an initial out-of-capacity address.

`extract_calibration` independently verifies the entire pinned update before
deriving its checked128-byte calibration extent. The resulting fingerprint is
`10fa7918107458a1b72e18ef91b9bfc27b6eb76e3b562aa6566c5561dcf4929a`.
The source is retained privately and never embedded in the published helper or
unit fixtures. All unit data is anonymous generated test content.

Coverage alone establishes no successful transfer, valid data eye, controller
readiness, calibration result or complete flash reconstruction. A peripheral
transaction still has to admit its full configuration and deliver the actual
bytes before reporting completion. [Public setup](public-setup.md) describes the
local firmware boundary; [PHY initialization](phy-startup.md) describes the
preceding diagnostic profile.
