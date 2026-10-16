The main idea is to create a programm for windows (later android and IOS) that would allow to combine tools similar to DESMOS, Mathlab, Derive etc. 

GUI:
1. Notepad-alike. You, as a user may define a notepad - a folder containing following subfiles: a chapter (collection of sheets), sheets. One sheet is a canvas, seemlesly separated into pages. It must be possible to define those page sizes, and they are A4 by default. The pages are connected seamlessly, only separated by thin lines on a visual layer and do not intervien with the users actions. One can write over them. They deem as visual guidelines to he user on how the pages can be separated dring exporting process. (During exportuser may define the order in which activated pages can be exported into the end file (pdf)). A page deems activated when something was and is written, pasted on it. When page contains no contents - either by them being erased - it is removed from the user defined export queue.
2. The sheets may be opened into a full screen mode to hyde the side panel, where the document is organized (see OneNote fo inspiration). 
3. The tools available currently to the user from a tool panel (located at the top of the window at all times and all modes) are: Draw freehand, Draw straight line, Select.
4. (To be added after implementing those features.)
5. Import PDFs: complete documents or chosen pages go into a new sheet, one page per PDF page, as a locked background to write over (annotate lecture slides, worksheets, papers). Exported again as part of the sheet.
