# Updates

[Documentation index](README.md) · [Profile backups](profile.md)

## Checking and choosing a channel

XLibrary checks this project's GitHub Releases at startup. A notification about a newer version links to **Settings → Update**.
You can also choose **Check for updates** there. Downloads and installations require confirmation.

The **Update channel** defaults to **Stable**, which considers published releases whose GitHub `prerelease` flag is false.
**Preview** also considers pre-releases. Versions use Semantic Versioning; downgrades are not offered.
Drafts and unsupported version tags are ignored. Switching channels cancels the current check/download and clears its available installer.

Results are cached for the current application session: 60 seconds without a token or 5 seconds with one, measured from completion.
Errors and cancellations use the same interval. Repeated clicks can show the cached result without another request;
reading the result does not extend its lifetime. Each channel and authentication identity has a separate cache entry.

## Installing an update

An installer-managed Windows x64 copy can download the matching MinGW or MSVC installer after confirmation.
XLibrary checks its size and GitHub-provided SHA-256 before opening the installation wizard in the existing installation directory and exiting.
The wizard completes the update and offers to reopen XLibrary. The existing profile remains in place.

Downloads can be cancelled. Closing settings cancels checks and downloads started there; the automatic startup check continues independently.

Portable copies, Linux installations, and releases without a matching installer and digest open the release in your browser after confirmation.
Follow the release notes for the appropriate ZIP, DEB, or AppImage. Keep the portable `data/` folder or AppImage's adjacent profile directory;
see [storage modes](profile.md#storage-modes). A complete profile backup is useful before upgrading.

## Optional GitHub token

Checks work without a token. You can optionally enter your own GitHub Personal Access Token in **Settings → Update**.
For this public repository, no additional repository permissions are needed.

**Save token** stores it in Windows Credential Manager or the Linux secret store (Secret Service/KWallet).
**Delete token** restores unauthenticated checks. These buttons act immediately, independently of the settings dialog's Save/Cancel buttons.
If the secret store is unavailable or locked, XLibrary reports an error and does not fall back to plaintext storage.

The token is sent only to `api.github.com`, including paginated release requests, and is not attached to installer downloads.
It is not written to `settings.ini`, catalog exports, or profile backups. Cache entries identify authentication contexts without storing the tokens themselves.
The saved credential belongs to the OS user and local profile path; enter it again after moving a portable profile.
