1. **Internal html viewer.**
Ability to view small (< 32 kb) html files without external resources in internal viewer. 
QLabel control is using. [Supported HTML Subset](https://doc.qt.io/qt-5/richtext-html-subset.html). To pen in external (system) viewer use ctrl+click on file.  
2. **Sending html as file.**
Ability to send html code as file, so recipient can view it as rendered html. For this, type:  
` ```html` _your html code here_  
Than, send it.

This 2 additions + some external tools allow you to send chunks of code to each other with syntax highlighting.  
Some of tools, that alow you to obtain syntax highlighting with html:  
- Visual studio plugin [Copy As Html](https://marketplace.visualstudio.com/items?itemName=VisualStudioPlatformTeam.CopyAsHtml).
- NppExport Notepad++ plugin (not compatible with QLabel - external view only :( ).
- Many on-line web services...  
![Example](assets/html.gif?raw=true "sending code with syntax highlighting")
