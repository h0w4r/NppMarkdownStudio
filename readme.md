# NppMarkdownStudio — Plugin Nativo de Markdown para Notepad++

**NppMarkdownStudio** es un plugin nativo en **C++ (Win32 API + Scintilla + Microsoft Edge WebView2)** para Notepad++ que combina un **panel acoplable (*dockable panel*) de vista previa en tiempo real** con una **suite completa de edición y formato Markdown**, superando las limitaciones de los plugins actuales como [MarkdownViewer++](https://www.google.com/search?q=https%3A%2F%2Fgithub.com%2Fnea%2FMarkdownViewerPlusPlus) y [NppMarkdownPanel](https://www.google.com/search?q=https%3A%2F%2Fgithub.com%2Fmohzy83%2FNppMarkdownPanel).

## Comparativa frente a los plugins existentes

| Característica | [MarkdownViewer++](https://www.google.com/search?q=https%3A%2F%2Fgithub.com%2Fnea%2FMarkdownViewerPlusPlus) | [NppMarkdownPanel](https://www.google.com/search?q=https%3A%2F%2Fgithub.com%2Fmohzy83%2FNppMarkdownPanel) | **NppMarkdownStudio (Este Plugin)** |
| :--- | :--- | :--- | :--- |
| **Arquitectura y Carga** | C# (.NET Legacy / IE11 `WebBrowser`, archivado en 2026) | C# (.NET 4.7.2 + múltiples DLLs externas en subcarpetas) | **C++17 Nativo (`NppMarkdownStudio.dll` único)** sin errores de `DllExport` |
| **Motor del Panel Acoplable** | Internet Explorer 11 (`WebBrowser`) | WebView2 / IE11 (solo visor pasivo) | **Panel nativo `NPPM_DMMREGASDCKDLG` + WebView2 interactivo** |
| **Sincronización de Scroll** | Solo unidireccional básica | Hacia el cursor o primera línea visible | **Bidireccional (`data-line`) + Salto al código con doble clic** |
| **Listas de Tareas (`- [ ]`)** | Solo lectura | Solo visual | **Interactivas**: marcar una casilla en la vista previa edita el texto en Scintilla con soporte de `Undo` |
| **Herramientas de Edición** | Ninguna (solo visor) | Ninguna (solo visor) | **10 comandos de edición + auto-continuación de listas + alineador de tablas GFM + generador de TOC** |
| **Archivos nuevos (`new 1`)** | Fallos de refresco (#142) | Requiere guardar archivo o ajuste manual (#186) | **Soporte inmediato para buffers sin guardar (`new 1`)** |
| **Sintaxis Extendida** | CommonMark básico | GFM + Mermaid básico | **GFM + Alertas GitHub (`[!NOTE]`) + YAML Frontmatter + Matemáticas + Mermaid + Notas al pie (`[^1]`)** |
| **Modo Oscuro Nativo** | No soportado | Requiere CSS estático separado | **Sincronización dinámica con la paleta exacta de Notepad++ (`NPPM_GETDARKMODECOLORS`)** |

## Funcionalidades Principales

1. **Panel Acoplable Nativo (`NPPM_DMMREGASDCKDLG`)**:
   - Se integra directamente en el administrador de paneles de Notepad++ (acoplable a la derecha, izquierda, arriba, abajo o flotante) y recuerda su estado entre sesiones.
   - Renderizado fluido con *debouncing* de 120 ms al escribir (`SCN_MODIFIED`) para evitar bloqueos en documentos extensos.
2. **Puente Bidireccional (`WebView2 <-> Scintilla`)**:
   - **Editor a Vista Previa**: Al desplazarte por el código fuente (`SCN_UPDATEUI`), el panel sincroniza su posición usando atributos `data-line` inyectados en cada bloque HTML.
   - **Vista Previa a Editor**: Hacer **doble clic** sobre cualquier párrafo, título, bloque de código o fila de tabla en la vista previa lleva el cursor de Notepad++ exactamente a esa línea (`SCI_GOTOLINE`).
   - **Checkboxes Interactivos**: Al marcar o desmarcar una tarea en la vista previa, el plugin actualiza `- [ ]` a `- [x]` directamente en el editor Scintilla dentro de una transacción `SCI_BEGINUNDOACTION` / `SCI_ENDUNDOACTION`.
3. **Asistente Inteligente de Edición Markdown**:
   - **Atajos de Formato**: Negrita (`Ctrl+B`), Cursiva (`Ctrl+I`), Código en línea o bloque (`Ctrl+Shift+K`), Enlace (`Ctrl+K`), Alternar tarea (`Ctrl+Shift+X`), Tachado, Imagen y Alertas GitHub (`> [!NOTE]`).
   - **Alineador y Formateador de Tablas GFM (`Ctrl+Alt+T`)**: Si seleccionas una tabla Markdown desalineada, calcula el ancho máximo de cada columna y rellena las celdas automáticamente. Si no hay selección, inserta una plantilla de tabla GFM.
   - **Generador de Tabla de Contenidos (`TOC`)**: Escanea todos los encabezados (`#` a `######`) del documento y genera un índice Markdown con enlaces de anclaje listos.
   - **Auto-continuación de Listas (`SCN_CHARADDED`)**: Al presionar `Enter` en una lista (`- `, `* `, `1. `, `- [ ] `), inserta automáticamente el siguiente ítem o limpia la línea si estaba vacía.
4. **Barra Superior en el Panel de Vista Previa**:
   - Contador en vivo de **palabras, caracteres, líneas y tiempo estimado de lectura**.
   - **Cajón de Esquema (Outline Drawer)** desplegable para navegar por las secciones del documento.
   - Botón **Copiar** en cada bloque de código y exportación directa a **HTML** y **PDF**.

## Cómo Compilar e Instalar

### Requisitos
- **Visual Studio 2022** (con la carga de trabajo *Desarrollo para el escritorio con C++*) y **CMake 3.20+**.
- (Opcional pero recomendado) Paquete NuGet `Microsoft.Web.WebView2` para habilitar el motor Chromium/Edge completo.

### Compilación con CMake (x64)
Ejecuta los siguientes comandos desde la carpeta donde descargues `NppMarkdownStudio.cpp`, `NppMarkdownStudio.h` y `CMakeLists.txt`:

```powershell
mkdir build
cd build
cmake .. -A x64
cmake --build . --config Release
```

### Instalación en Notepad++
1. Abre la carpeta de plugins de tu instalación de Notepad++:
   `C:\Program Files\Notepad++\plugins\`
2. Crea una subcarpeta con el nombre exacto del DLL:
   `C:\Program Files\Notepad++\plugins\NppMarkdownStudio\`
3. Copia el archivo compilado `NppMarkdownStudio.dll` dentro de esa carpeta:
   `C:\Program Files\Notepad++\plugins\NppMarkdownStudio\NppMarkdownStudio.dll`
4. Reinicia Notepad++ y abre el panel desde **Plugins -> NppMarkdownStudio -> Mostrar/Ocultar Panel Markdown** (o presiona `Ctrl+Shift+M`).
