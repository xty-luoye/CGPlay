# CGPlay binary release checklist

This historical filename is retained for existing links. The checklist applies
to both free and paid binary distribution; CGPlay-authored source uses the GNU
General Public License version 3 only (GPL-3.0-only), whose full text is in the
root `LICENSE.txt`. Paid distribution is permitted under its terms.

## Source release

- Include the root `LICENSE.txt` and retain third-party LICENSE/NOTICE files.
- Identify external dependencies and local patches without claiming ownership
  of upstream code.
- Exclude private settings, credentials, build outputs, installed runtimes,
  and media whose redistribution rights have not been established.

## Before publishing an installer

- Include `LICENSE.txt`, `licenses/THIRD_PARTY_NOTICES.txt`,
  `licenses/FFMPEG_SOURCE_OFFER.txt`, and applicable third-party notices.
- Inventory the exact DLL, EXE, and Python package versions in the payload.
  The linked FFmpeg DLLs and standalone FFmpeg executables are different builds.
- Provide corresponding source, configuration, patches, and build information
  required by the exact FFmpeg and Qt/Chromium licenses. Verify recipients can
  obtain the material; a general upstream link alone is not verification.
- Preserve applicable LGPL library replacement/relinking rights and notices.
- Review the runtime dependencies actually shipped, including Qt WebEngine,
  Python packages, and any optional Codex runtime. Source licensing does not
  grant access to third-party services or replace their terms.
- Run `tools/validate_commercial_release.ps1` against the staged directory.
  Despite its historical name, it checks basic license-document presence only.
  A passing report is not a corresponding-source or legal-compliance approval.
- Review any separately applicable rights for included media, models, brands,
  and codecs in the intended distribution.

## Current publication scope

This source-only publication does not include or newly approve an installer.
The source locations recorded in `FFMPEG_SOURCE_OFFER.txt` are provenance leads;
completeness and accessibility of matching source must be verified separately
before attaching binary releases to this repository.
