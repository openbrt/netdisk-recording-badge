**English** · [简体中文](kuku-badge-user-guide.zh_CN.md)

# Netdisk Recording Badge User Guide

This guide corresponds to the on-device User Guide menu. The screen labels the confirm button “OK” and uses one short action or status per line. Update this file, its Chinese counterpart, and the on-device guide in the same iteration whenever features or controls change.

## Basic controls

- If the display is off, press any button to wake it first; that gesture only wakes the display. On the badge home screen, tap OK to open the menu. Use Up and Down to choose an item and OK to enter. Hold OK to return from a child page.
- The User Guide has four pages. Its page count appears at the top of the content panel, and the page controls appear inside the panel. Press Down for the next page, Up for the previous page, or hold OK to return home.
- The top bar shows Wi-Fi, cloud, recording, and battery status. A discreet dot indicates recording, and the home screen shows the session duration.
- Hold Down on the home screen to turn off the display; press any button to wake it.

## Recording

- Press Down and OK together on any page, including with the display off, to request recording. Holding OK on the home screen also requests recording.
- Recording and upload can run together. Only closed recordings are uploaded; pending files and active uploads do not block a new recording. Starting needs at least 256 KB free; low space saves the current segment and stops with a clear message; unsynced recordings are never overwritten. During an active upload, the badge reuses its network/authorization state instead of opening another cloud probe.
- The badge checks Wi-Fi and cloud availability before starting. If either is unavailable, it shows a prompt and does not start recording. A disconnect saves the current segment and stops the session. Restoring connectivity uploads saved files but does not restart recording; press the button to start a new session.
- Hold OK while recording to stop. The session saves a segment every 60 seconds and uploads it in the background. File rotation requires no button press and keeps the microphone capture worker running. Manual stop saves the final partial segment; interrupted transfers retry after connectivity returns. Pending local files appear at the top of the root file list with their status.
- Uploads go to `YYYY-MM-DD/` subfolders of the application folder, using the recording start date in Beijing time. Retries keep that original date. Unsynchronized `REC` files go to `undated/`; existing files at the root are not moved.
- Select recordings in Baidu Netdisk KuKu AI for transcription or summaries. Use the date folders to find a group of recordings and specify which files to process. There is no two-minute session limit. Session length depends on battery life, Netdisk availability and upload speed. A network loss stops and saves the session. Slow uploads that fill local storage also cause a safe stop. After a power loss, the next boot attempts to repair the final unclosed recording. Device checks have verified multi-minute recording, automatic rotation, manual stop, date-folder uploads and downloaded WAV integrity. Multi-hour sessions, physical network/power loss and audio quality across segment boundaries remain unverified.

## Cloud files and pictures

- Open Cloud Files from the menu. If authorization is needed, scan the displayed authorization code. Authorized files are listed in descending modification-time order. Select a date folder and press OK to enter; hold OK to go up one level. At the root, hold OK to return home.
- A successfully loaded list stays stable while you browse. To see new files, leave and re-enter the directory. Failed reads still retry automatically.
- Use Up and Down to select a file; hold either button to move faster. Press OK on a local recording to play it. During playback, OK stops and Up or Down changes volume.
- Select a JPG to view it. The badge fits the entire image within the near-full-width preview, preserving its proportions and leaving the status bar visible. While viewing, tap OK to save a small, complete portrait for the badge, or hold OK to return to the file list.
- If the image is a share QR created by the cloud client, the badge displays that image itself. Whether another person's camera can download the file depends on the embedded share link and its permissions and expiry.
- Other cloud files currently show an example share QR. General sharing still requires service authorization from the project author.

## Reset the Netdisk connection

- Open **Reset Netdisk connection** from the menu. **Cancel** is selected by default; use UP/DOWN to select **Confirm reset**, then click OK. Hold OK to leave.
- Reset clears only the badge's saved Netdisk authorization and file-list cache. Wi-Fi credentials, the badge portrait, local recordings and cloud files remain. This does not revoke the application's permission in the Baidu account.
- Reset is refused during recording, playback or file transfer/loading; finish first and retry. An authorization waiting for a scan is cancelled safely. When **Connection reset** appears, click OK for a fresh QR code and authorize on the phone. The QR code disappears automatically after authorization and the cloud-file list opens.

## Wi-Fi

- Open Wi-Fi, press OK to scan, select a network, enter its password on the badge keyboard, then hold OK to connect.
- To cancel, select **<** with Up/Down and tap OK until the password is empty, then tap **<** once more to return to the scan list. Holding OK connects; it does not cancel. Hold OK on the scan page to leave and restore the saved network connection.
- The badge remembers multiple networks and reconnects automatically. Press Down on the Wi-Fi page to manage saved networks. The list shows only saved entries, starting at the top when six or fewer are saved. Select one with OK, then press OK again on the confirmation page to delete it; hold OK on that page to cancel and keep the network.

The passive NFC tag is independent of the display and recording firmware. Access-control compatibility depends on whether a reader accepts the tag's identifier and protocol.
