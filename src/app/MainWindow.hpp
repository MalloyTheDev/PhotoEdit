#pragma once

#include "BusyTask.hpp"  // pe::app::TaskAccess / TaskResult
#include "Theme.hpp"

#include "pe/core/Document.hpp"  // pe::DocumentObserver (base class)
#include "pe/core/DocumentIO.hpp"
#include "pe/core/Layer.hpp"  // pe::LayerId
#include "pe/core/Refusal.hpp"
#include "pe/core/Selection.hpp"

#include <QMainWindow>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class QAction;
class QCloseEvent;
class QLabel;
class QMenu;
class QPointF;
class QSpinBox;
class QToolBar;
class QToolButton;
class QWidget;

namespace pe {
class Adjustment;                          // only ever held by pointer here, so a declaration
enum class AdjustmentKind : std::uint8_t;  // does; Adjustment.hpp stays out of this header
}  // namespace pe

namespace pe::app {

class CanvasView;
class LayersPanel;
class HistoryPanel;
class ColorPanel;
class SwatchesPanel;
class AdjustmentsPanel;
class ChannelsPanel;
class PropertiesPanel;

// What the user chose when asked about unsaved changes.
enum class DiscardAnswer { Save, Discard, Cancel };

// May the caller go ahead and replace or close the document?
//
// The RULE, with the dialog and the save injected, so it can be tested without a modal box.
// It is small and it was wrong: it used to return true as soon as a save succeeded, which
// is not the same question. A save serializes a SNAPSHOT and leaves the canvas live, so the
// user can paint while it writes; those strokes are correctly still unsaved afterwards, and
// answering "yes, discard" on the strength of "the save worked" destroyed them with no
// second prompt. So it asks again while `isDirty` still says there is something to lose.
//
// `save` returning false (a failed write, or a Save As the user cancelled) aborts rather
// than looping, or a broken disk would trap the user in the prompt.
[[nodiscard]] bool resolveUnsavedChanges(const std::function<bool()>& isDirty,
                                         const std::function<DiscardAnswer()>& ask,
                                         const std::function<bool()>& save);

// Why pe::saveDocument() refused, phrased for a dialog. Three cases the app can tell
// apart: a canvas too large to flatten to a raster format (every raster format goes
// through compositeImage(), which returns nothing above kMaxCompositeImagePixels, so
// such a save can never succeed and "Could not save" sends the user to check disk
// permissions instead), an extension this build cannot write, and an IO failure.
//
// A free function rather than a member because it is a pure function of the document
// and the path, which also makes it testable without opening up MainWindow.
[[nodiscard]] QString saveFailureReason(const pe::Document* doc, const QString& path,
                                        pe::SaveError err);

// Why pe::loadDocument() returned nullptr, phrased for a dialog. Six distinct failures
// used to collapse into one generic line, so a user denied read access saw the same text
// as one opening a corrupt file.
[[nodiscard]] QString openFailureReason(const QString& path, pe::LoadError err);

// Why an adjustment layer's parameters cannot be edited. Two different situations that one
// message used to conflate: Invert HAS no parameters, so there is nothing a dialog could
// offer, while a kind whose editor is simply not written yet will grow one. Telling someone
// that inverting "isn't supported yet" is false; it is supported, it is just not adjustable,
// and the Adjustments panel makes that layer one click away.
[[nodiscard]] QString noAdjustmentEditorReason(pe::AdjustmentKind kind, const QString& name);

// The top-level application window. Wires the menus to the engine's document I/O and
// shows the active document on a CanvasView. Color, Swatches, Adjustments, Properties,
// Layers and History are real panels; Gradients, Patterns, Libraries, Channels and Paths
// are still placeholders. See docs/systems/24-ui-workspace.md.
class MainWindow : public QMainWindow, public pe::DocumentObserver {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    // Guards the window close against unsaved work. File > Exit routes through
    // close() so this runs for both paths.
    void closeEvent(QCloseEvent* e) override;

    // The title carries the modified marker, so it has to track the dirty bit rather
    // than only being refreshed when the document is swapped.
    void onDocumentChanged(const pe::Document& doc, const pe::DocumentChange& change) override;

private:
    // Offers Save / Discard / Cancel when the current document has unsaved changes.
    // Returns false only if the user cancels, in which case the caller must abort.
    // True when there is nothing to lose, when the user saved, or when they discarded.
    [[nodiscard]] bool confirmDiscard();
    // Shows the unsaved-changes prompt. Split out so confirmDiscard's rule is the part
    // under test and the modal box is the part that is not; see resolveUnsavedChanges.
    [[nodiscard]] DiscardAnswer askAboutUnsavedChanges();

    void buildMenuBar();
    void buildToolBar();
    void buildOptionsBar();
    void buildCentral();
    void buildDockPanels();
    void buildStatusBar();
    void setTheme(ThemeId id);
    // Fills the Window menu with one toggle per dock. Must run after buildDockPanels,
    // since the toggles come from the docks themselves.
    void populateWindowMenu();
    void showAbout();
    // Canvas dimensions and the zoom readout in the options bar. Driven by the
    // document swap and by CanvasView::zoomChanged.
    void refreshZoomStrip();
    // Bundled glyphs are tinted when rendered, so a theme change has to rebuild them.
    void retintIcons();
    // Enables or disables everything that needs an open document, and keeps Undo and
    // Redo matching the history. Nothing was ever disabled before, so refusing an
    // operation was indistinguishable from it being broken.
    void updateActionStates();
    void clearCursorPos();  // blanks the position readout when the cursor is off-canvas

    // Which contextual control group the options bar shows for the active tool.
    enum class OptKind { None, Brush, Wand, Move };
    void updateOptionsBar(OptKind kind, const QString& toolName);
    void refreshDocTab();
    [[nodiscard]] QWidget* makeColorSwatches();
    void chooseForegroundColor();
    void chooseBackgroundColor();
    void updateSwatches();

    void newDocument();
    void openDocument();
    bool saveDocumentAs();    // always prompts
    void exportDocumentAs();  // flatten + encode to a raster format with per-format options
    void onAddText(const QPointF& docPos);  // Type tool: prompt + rasterize + stamp text
    // Open the live-preview parameter dialog for an adjustment layer (double-click in the Layers
    // panel, or Layer▸Edit Adjustment). Commits one EditAdjustmentCommand on OK; a no-op for a
    // non-adjustment layer or a type without an editor yet.
    void editAdjustmentLayer(pe::LayerId id);
    // Add one adjustment layer on top of the stack as a single undo step and make it active.
    // The only path: Layer▸New Adjustment Layer and the Adjustments panel both come through
    // here, so where the layer lands and what it is called cannot differ between them.
    void addAdjustmentLayer(std::unique_ptr<pe::Adjustment> adj, const QString& name);
    // Reopen the text dialog for a text layer (double-click in the Layers panel), seeded from its
    // model; commits one EditTextCommand on OK. A no-op for a non-text layer.
    void editTextLayer(pe::LayerId id);
    bool writeTo(const QString& path);

public:
    // Record and show a refused operation. Every refusal in the shell goes through here,
    // so there is one place that decides how a refusal is presented and one place a test
    // can look. See pe::Refusal for why this carries structure rather than a message.
    // The window's own operations, reachable from the menus and the shortcuts. Public
    // because that is what they are, not merely so tests can call them: a test drives the
    // same entry point the user does rather than a private helper behind it.
    void undo();
    void redo();
    void setDocument(std::unique_ptr<pe::Document> doc, QString path);
    // Saves to the current path, or prompts if there is none. Public for the same reason
    // as undo/redo: File > Save is a window operation, and the whole of it now runs on a
    // worker thread, so a test that drives anything smaller would not exercise the part
    // that can go wrong.
    bool saveDocument();

    void reportRefusal(const pe::Refusal& r);

    // Selection refinements, separated from the dialogs that prompt for their amounts so
    // the refusal path is reachable without a modal.
    void growSelection(int px);
    void shrinkSelection(int px);
    void featherSelection(float radius);

    // True while a background document task (a save, an export, an open) is running. The
    // File operations are disabled for the duration; a snapshot task deliberately leaves
    // everything else, including painting, available.
    [[nodiscard]] bool documentTaskInFlight() const noexcept { return documentTaskInFlight_; }

    // The open document, or null. Read-only borrow: MainWindow owns it.
    [[nodiscard]] pe::Document* document() const noexcept { return doc_.get(); }
    [[nodiscard]] CanvasView* canvas() const noexcept { return canvas_; }

    // The refusals seen so far, newest last. For tests: an assertion on a status-bar
    // string would break on any wording change, and could not tell a refusal apart from
    // any other transient message.
    [[nodiscard]] const std::vector<pe::Refusal>& refusals() const noexcept { return refusals_; }
    [[nodiscard]] pe::RefusalCode lastRefusalCode() const noexcept {
        return refusals_.empty() ? pe::RefusalCode::None : refusals_.back().code;
    }
    void clearRefusals() { refusals_.clear(); }

private:
    void refreshTitle();
    // Refuse `operation` and return false, so a guard reads as one expression:
    //     if (!requireActiveLayer(...)) return;
    // Run `work` as a background document task with the File operations disabled for its
    // duration. Every long operation in this window goes through here rather than calling
    // runDocumentTask directly, so the re-entrancy guard cannot be forgotten at one call
    // site. `access` states what the work may reach; see BusyTask.hpp.
    [[nodiscard]] TaskResult runGuardedTask(const QString& title, TaskAccess access,
                                            const std::function<void()>& work);

    [[nodiscard]] bool refuseIf(bool condition, const char* operation, pe::RefusalCode code,
                                const char* action, const QString& explanation);
    [[nodiscard]] QString describeState() const;  // the context field: what was true
    void refineSelection(const char* action, const std::function<void(pe::Selection&)>& apply);
    void onColorPicked(const QColor& c);
    // The single place the foreground colour changes. The eyedropper, the colour dialog, the
    // Color panel and the Swatches grid all arrive here, so the brush, the tool-strip swatch,
    // the picker and the palette cannot end up disagreeing about what colour is loaded.
    void setForegroundColor(const QColor& c);

    std::unique_ptr<pe::Document> doc_;
    CanvasView* canvas_ = nullptr;
    LayersPanel* layers_ = nullptr;
    HistoryPanel* history_ = nullptr;
    ColorPanel* colorPanel_ = nullptr;
    SwatchesPanel* swatchesPanel_ = nullptr;
    AdjustmentsPanel* adjustments_ = nullptr;
    ChannelsPanel* channels_ = nullptr;
    PropertiesPanel* properties_ = nullptr;
    QLabel* toolLabel_ = nullptr;        // status bar: active tool
    QLabel* toolHintLabel_ = nullptr;    // status bar: what the active tool does
    QLabel* posLabel_ = nullptr;         // status bar: cursor position in document pixels
    QLabel* zoomLabel_ = nullptr;        // status bar: zoom percentage
    QLabel* canvasSizeLabel_ = nullptr;  // options bar: canvas dimensions
    QLabel* zoomValueLabel_ = nullptr;   // options bar: zoom percentage

    // Icon-bearing widgets and the glyph each one shows, so retintIcons() can rebuild
    // them when the theme changes.
    struct ThemedIcon {
        QAction* action;
        QString name;
        int size;
    };
    struct ThemedButton {
        QToolButton* button;
        QString name;
        int size;
    };
    std::vector<ThemedIcon> themedIcons_;
    std::vector<ThemedButton> themedButtons_;

    // Menus and actions that need an open document. Held so updateActionStates()
    // can gate them in one place rather than each call site guarding itself silently.
    std::vector<pe::Refusal> refusals_;  // see refusals()
    std::vector<QMenu*> docMenus_;
    std::vector<QAction*> docActions_;
    // Every File-menu action, including the ones that work with no document open. They are
    // disabled for the duration of a background document task: a second save, or a New /
    // Open / Exit that replaces or destroys the document, would leave the in-flight task's
    // completion path (markSavedAt, currentPath_, the status message) pointing at a
    // document that no longer exists. A snapshot save deliberately leaves the CANVAS live,
    // so this is the guard that replaces blanket input blocking.
    std::vector<QAction*> fileActions_;
    bool documentTaskInFlight_ = false;
    QAction* undoAct_ = nullptr;
    QAction* redoAct_ = nullptr;

    QMenu* windowMenu_ = nullptr;        // View-style panel toggles, filled after docks exist
    QToolBar* optionsBar_ = nullptr;     // contextual tool options (top)
    QLabel* optToolName_ = nullptr;      // options bar: active tool name
    QWidget* brushOptions_ = nullptr;    // options bar: brush size/opacity group
    QWidget* wandOptions_ = nullptr;     // options bar: magic-wand group
    QAction* brushOptAction_ = nullptr;  // toolbar action wrapping brushOptions_ (for show/hide)
    QAction* wandOptAction_ = nullptr;   // toolbar action wrapping wandOptions_
    QWidget* moveOptions_ = nullptr;     // Move: Auto-Select + Show Transform Controls
    QAction* moveOptAction_ = nullptr;   // toolbar action wrapping moveOptions_
    // The tool-strip action for each wired tool, so a tool the CANVAS selected can check the
    // matching button. Keyed by the Tool enum value.
    QHash<int, QAction*> toolActions_;
    QActionGroup* toolGroup_ = nullptr;  // the strip's exclusive selection
    // Checkable, so the menu shows whether the ACTIVE layer is clipped rather than being a
    // one-way switch. Kept honest by updateActionStates().
    QAction* clipAct_ = nullptr;
    QSpinBox* sizeSpin_ = nullptr;        // options bar: brush diameter
    QSpinBox* opacitySpinOpt_ = nullptr;  // options bar: brush opacity
    QSpinBox* flowSpin_ = nullptr;        // options bar: brush flow
    QSpinBox* stabSpin_ = nullptr;        // options bar: stroke stabilization
    // Show the ACTIVE tool's brush settings. They are kept per tool, so switching tools
    // changes them under the options bar and the boxes have to catch up or they report the
    // previous tool's numbers while the new one paints with its own.
    void refreshBrushOptions();
    QSpinBox* wandTolSpin_ = nullptr;  // options bar: magic-wand tolerance
    QLabel* docTab_ = nullptr;         // document tab strip above the canvas
    QToolButton* fgSwatch_ = nullptr;  // foreground color swatch (tool strip)
    QToolButton* bgSwatch_ = nullptr;  // background color swatch (tool strip)
    QColor fgColor_;                   // current foreground (paint) color
    QColor bgColor_;                   // current background color

    QString currentPath_;
};

}  // namespace pe::app
