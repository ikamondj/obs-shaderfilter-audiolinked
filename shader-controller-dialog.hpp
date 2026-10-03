#pragma once

class QDialog;
class QWidget;

// UI thread only; parent owns the dialog, which also deletes itself on close.
QDialog *sf_create_controller_dialog(QWidget *parent);
