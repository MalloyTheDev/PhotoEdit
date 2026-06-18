#pragma once

#include "pe/core/DocumentIO.hpp"  // pe::ImageFormat, pe::ExportOptions

#include <QDialog>

class QComboBox;
class QLabel;
class QSlider;
class QSpinBox;
class QWidget;

namespace pe {
class Document;
}

namespace pe::app {

// Modal "Export As" dialog: choose a raster output format and its per-format options (today
// only JPEG, which exposes a quality slider) with a live estimated file size. The lossless
// formats (PNG/TIFF/WebP) have no adjustable options and show a short note instead.
//
// Usage: construct, exec(); on QDialog::Accepted read selectedFormat() + options(), then pick a
// path and write via pe::saveDocument(doc, path, options). The dialog re-encodes the document
// for the size estimate only on "settled" interactions (format change, slider release), never
// per slider tick, so it does not thrash on a large canvas.
class ExportDialog : public QDialog {
    Q_OBJECT

public:
    ExportDialog(QWidget* parent, const pe::Document& doc, pe::ImageFormat initialFormat);

    [[nodiscard]] pe::ImageFormat selectedFormat() const;
    [[nodiscard]] pe::ExportOptions options() const;

private:
    void onFormatChanged();
    void updateEstimate();  // re-encode at the current settings and show the byte size

    const pe::Document& doc_;  // not owned; outlives this modal dialog
    QComboBox* formatCombo_ = nullptr;
    QWidget* qualityRow_ = nullptr;  // shown only for JPEG
    QSlider* qualitySlider_ = nullptr;
    QSpinBox* qualitySpin_ = nullptr;
    QLabel* losslessNote_ = nullptr;  // shown for PNG/TIFF/WebP
    QLabel* sizeLabel_ = nullptr;
    bool syncing_ = false;  // guard: programmatic slider<->spin sync must not re-enter
};

}  // namespace pe::app
