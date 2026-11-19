# Gallery and clipboard shelf

The main window has a filename search, media filters and a persistent clipboard shelf.

- Ctrl+F focuses the filename search. Matching ignores case and supports Unicode names.
- All, Photos, GIF, Video and Shelf narrow the gallery. GIF is separate from still images.
- The date menu switches between newest first and oldest first. The choice survives reopening the window and restarting Zipshot.
- Ctrl+V or Paste adds a clipboard image or copied media files to Shelf. Text pasted into the search field remains text.
- F5 refreshes the file list. Captures saved by Zipshot appear in the active date order.

Shelf stores local copies in the Clipboard subfolder of the configured screenshots folder. Images remain available after restarting the app until they are deleted. There is no automatic clipboard monitoring, upload or timed deletion. Deleting a shelf copy uses the normal gallery file deletion action and does not delete its original source file.

Bitmap pastes are saved as lossless PNG with transparency. Copied image, GIF and video files keep their original bytes and animation. Duplicate filenames receive numbered alternatives. New files appear only after the copy or PNG encoding has completed.

Search and filtering use the complete scanned capture catalog rather than the former 500-file subset. Thumbnails are still loaded on demand with a bounded cache. Selection stays attached to file paths when sorting or adding captures; files hidden by a filter are removed from the selection.

The thumbnail and card actions continue to support opening, editing still images, copying, dragging and combining selected images. Searching and changing the type filter can be used together.
