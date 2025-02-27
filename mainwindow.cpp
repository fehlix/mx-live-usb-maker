/**********************************************************************
 *  mainwindow.cpp
 **********************************************************************
 * Copyright (C) 2017 MX Authors
 *
 * Authors: Adrian
 *          MX Linux <http://mxlinux.org>
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this package. If not, see <http://www.gnu.org/licenses/>.
 **********************************************************************/

#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QDebug>
#include <QFileDialog>
#include <QScrollBar>

#include "about.h"
#include "unistd.h"
#include <chrono>

using namespace std::chrono_literals;

extern const QString starting_home;

MainWindow::MainWindow(const QStringList &args, QDialog *parent)
    : QDialog(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setGeneralConnections();
    // setup options
    LUM.clear();
    QSettings settings(QStringLiteral("/etc/CUSTOMPROGRAMNAME/CUSTOMPROGRAMNAME.conf"), QSettings::NativeFormat);
    LUM = settings.value(QStringLiteral("LUM"), "live-usb-maker").toString();
    size_check = settings.value(QStringLiteral("SizeCheck"), 128).toUInt(); // in GB
    qDebug() << "LUM is:" << LUM;

    setWindowFlags(Qt::Window); // for the close, min and max buttons
    setup();
    ui->comboUsb->addItems(buildUsbList());
    if (args.size() > 1 && args.at(1) != QLatin1String("%f")) {
        ui->pushSelectSource->setText(args.at(1));
        ui->pushSelectSource->setProperty("filename", args.at(1));
        ui->pushSelectSource->setToolTip(args.at(1));
        ui->pushSelectSource->setIcon(QIcon::fromTheme(QStringLiteral("media-cdrom")));
        ui->pushSelectSource->setStyleSheet(QStringLiteral("text-align: left;"));
        setDefaultMode(args.at(1));
    }

    this->adjustSize();
}

MainWindow::~MainWindow() { delete ui; }

bool MainWindow::checkDestSize()
{
    const quint64 disk_size = cmd.getCmdOut("blockdev --getsize64 /dev/" + device).toULongLong() / (1024 * 1024 * 1024);

    if (disk_size > size_check) // question when writing on large drives (potentially unintended)
        return (QMessageBox::Yes
                == QMessageBox::question(
                    this, tr("Confirmation"),
                    tr("Target device %1 is larger than %2 GB. Do you wish to proceed?").arg(device).arg(size_check),
                    QMessageBox::No | QMessageBox::Yes, QMessageBox::No));
    else
        return true;
}

bool MainWindow::isRunningLive()
{
    const QString test = cmd.getCmdOut(QStringLiteral("df -T / |tail -n1 |awk '{print $2}'"), true);
    return (test == QLatin1String("aufs") || test == QLatin1String("overlay"));
}

bool MainWindow::isToRam() { return QFileInfo::exists(QStringLiteral("/live/config/did-toram")); }

void MainWindow::makeUsb(const QString &options)
{
    device = ui->comboUsb->currentText().split(QStringLiteral(" ")).at(0);

    QString source;
    QString source_size;
    if (!ui->checkCloneLive->isChecked() && !ui->checkCloneMode->isChecked()) {
        source = "\"" + ui->pushSelectSource->property("filename").toString() + "\"";
        source_size = cmd.getCmdOut(
            "du -m \"" + ui->pushSelectSource->property("filename").toString() + "\" 2>/dev/null |cut -f1", true);
    } else if (ui->checkCloneMode->isChecked()) {
        source_size = cmd.getCmdOut("du -m --summarize \"" + ui->pushSelectSource->property("filename").toString()
                                        + "\" 2>/dev/null |cut -f1",
                                    true);
        source = "clone=\"" + ui->pushSelectSource->property("filename").toString() + "\"";
        // check if source and destination are on the same drive
        QString root_partition
            = cmd.getCmdOut("df --output=source \"" + ui->pushSelectSource->property("filename").toString()
                            + "\" |awk 'END{print $1}'");
        if ("/dev/" + device == cmd.getCmdOut(cli_utils + "get_drive " + root_partition)) {
            QMessageBox::critical(this, tr("Failure"),
                                  tr("Source and destination are on the same device, please select again."));
            ui->stackedWidget->setCurrentWidget(ui->selectionPage);
            ui->pushNext->setEnabled(true);
            setCursor(QCursor(Qt::ArrowCursor));
            return;
        }
    } else if (ui->checkCloneLive->isChecked()) {
        source = QStringLiteral("clone");
        if (isToRam())
            source_size = cmd.getCmdOut(QStringLiteral("du -m --summarize /live/to-ram 2>/dev/null |cut -f1"), true);
        else
            source_size = cmd.getCmdOut(QStringLiteral("du -m --summarize /live/boot-dev 2>/dev/null |cut -f1"), true);
    }

    if (!checkDestSize()) {
        ui->stackedWidget->setCurrentWidget(ui->selectionPage);
        ui->pushNext->setEnabled(true);
        setCursor(QCursor(Qt::ArrowCursor));
        return;
    }

    // check amount of io on device before copy, this is in sectors
    const quint64 start_io = cmd.getCmdOut("cat /sys/block/" + device + "/stat |awk '{print $7}'", true).toULongLong();
    ui->progBar->setMinimum(start_io);
    qDebug() << "start io is " << start_io;
    const quint64 iso_sectors = source_size.toULongLong() * 2048; // source_size * 1024 / 512 * 1024
    ui->progBar->setMaximum(static_cast<int>(iso_sectors + start_io));
    qDebug() << "max progress bar is " << ui->progBar->maximum();

    QString cmdstr = QString(LUM + " gui " + options + "-C off --from=%1 -t /dev/%2").arg(source, device);
    if (ui->radioDd->isChecked()) {
        cmdstr = LUM + " gui partition-clear -NC off --target " + device;
        connect(&cmd, &QProcess::readyRead, this, &MainWindow::updateOutput);
        qDebug() << cmd.getCmdOut(cmdstr);
        cmdstr = "dd bs=1M if=" + source + " of=/dev/" + device;
        ui->outputBox->appendPlainText(tr("Writing %1 using 'dd' command to /dev/%2,\n\n"
                                          "Please wait until the the process is completed")
                                           .arg(source, device));
    }
    setConnections();
    stat_file = new QFile("/sys/block/" + device + "/stat");
    qDebug() << cmd.getCmdOut(cmdstr);
}

void MainWindow::setup()
{
    connect(QApplication::instance(), &QApplication::aboutToQuit, this, &MainWindow::cleanup);
    this->setWindowTitle(QStringLiteral("Custom_Program_Name"));

    QFont font(QStringLiteral("monospace"));
    font.setStyleHint(QFont::Monospace);
    ui->outputBox->setFont(font);

    ui->groupAdvOptions->hide();
    advancedOptions = false;
    ui->pushBack->hide();
    ui->stackedWidget->setCurrentIndex(0);
    ui->pushCancel->setEnabled(true);
    ui->pushNext->setEnabled(true);
    ui->outputBox->setCursorWidth(0);
    height = this->heightMM();

    QRegularExpression rx(QStringLiteral("\\w*"));
    QValidator *validator = new QRegularExpressionValidator(rx, this);
    ui->textLabel->setValidator(validator);

    // set save boot directory option to disable unless update mode is checked
    ui->checkSaveBoot->setEnabled(false);

    // check if running live
    ui->checkCloneLive->setEnabled(isRunningLive());

    // disable clone running live system when booted encrypted
    if (QFile::exists(QStringLiteral("/live/config/encrypted")))
        ui->checkCloneLive->setEnabled(false);

    // check if datafirst option is available
    if (!cmd.run(LUM + " --help |grep -q data-first", true)) {
        ui->comboBoxDataFormat->hide();
        ui->checkDataFirst->hide();
        ui->spinBoxDataSize->hide();
        ui->labelFormat->hide();
    }
}

void MainWindow::setGeneralConnections()
{
    connect(ui->checkCloneLive, &QCheckBox::clicked, this, &MainWindow::checkCloneLive_clicked);
    connect(ui->checkCloneMode, &QCheckBox::clicked, this, &MainWindow::checkCloneMode_clicked);
    connect(ui->checkDataFirst, &QCheckBox::clicked, this, &MainWindow::checkDataFirst_clicked);
    connect(ui->checkUpdate, &QCheckBox::clicked, this, &MainWindow::checkUpdate_clicked);
    connect(ui->pushAbout, &QPushButton::clicked, this, &MainWindow::pushAbout_clicked);
    connect(ui->pushBack, &QPushButton::clicked, this, &MainWindow::pushBack_clicked);
    connect(ui->pushCancel, &QPushButton::clicked, this, &MainWindow::close);
    connect(ui->pushHelp, &QPushButton::clicked, this, &MainWindow::pushHelp_clicked);
    connect(ui->pushLumLogFile, &QPushButton::clicked, this, &MainWindow::pushLumLogFile_clicked);
    connect(ui->pushNext, &QPushButton::clicked, this, &MainWindow::pushNext_clicked);
    connect(ui->pushOptions, &QPushButton::clicked, this, &MainWindow::pushOptions_clicked);
    connect(ui->pushRefresh, &QPushButton::clicked, this, &MainWindow::pushRefresh_clicked);
    connect(ui->pushSelectSource, &QPushButton::clicked, this, &MainWindow::pushSelectSource_clicked);
    connect(ui->radioDd, &QRadioButton::clicked, this, &MainWindow::radioDd_clicked);
    connect(ui->radioNormal, &QRadioButton::clicked, this, &MainWindow::radioNormal_clicked);
    connect(ui->spinBoxSize, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::spinBoxSize_valueChanged);
    connect(ui->textLabel, &QLineEdit::textChanged, this, &MainWindow::textLabel_textChanged);
}

// Build the option list to be passed to live-usb-maker
QString MainWindow::buildOptionList()
{
    QString options(QStringLiteral("-N "));
    if (ui->checkEncrypt->isChecked())
        options += QLatin1String("-E ");
    if (ui->checkGpt->isChecked())
        options += QLatin1String("-g ");
    if (ui->checkKeep->isChecked())
        options += QLatin1String("-k ");
    if (ui->checkPretend->isChecked()) {
        options += QLatin1String("-p ");
        if (ui->sliderVerbosity->value() == 0) // add Verbosity of not selected, workaround for LUM bug
            ui->sliderVerbosity->setSliderPosition(1);
    }
    if (ui->checkSaveBoot->isChecked())
        options += QLatin1String("-S ");
    if (ui->checkUpdate->isChecked())
        options += QLatin1String("-u ");
    if (ui->checkSetPmbrBoot->isChecked())
        options += QLatin1String("--gpt-pmbr ");
    if (ui->spinBoxEsp->value() != 50)
        options += "--esp-size=" + ui->spinBoxEsp->cleanText() + " ";
    if (ui->spinBoxSize->value() < ui->spinBoxSize->maximum())
        options += "--size=" + ui->spinBoxSize->cleanText() + " ";
    if (!ui->textLabel->text().isEmpty())
        options += " --label=" + ui->textLabel->text() + " ";
    if (ui->checkForceUsb->isChecked())
        options += QLatin1String("--force=usb ");
    if (ui->checkForceAutomount->isChecked())
        options += QLatin1String("--force=automount ");
    if (ui->checkForceMakefs->isChecked())
        options += QLatin1String("--force=makefs ");
    if (ui->checkForceNofuse->isChecked())
        options += QLatin1String("--force=nofuse ");
    if (ui->checkDataFirst->isChecked())
        options
            += "--data-first=" + ui->spinBoxDataSize->cleanText() + "," + ui->comboBoxDataFormat->currentText() + " ";

    if (ui->sliderVerbosity->value() == 1)
        options += QLatin1String("-V ");
    else if (ui->sliderVerbosity->value() == 2)
        options += QLatin1String("-VV ");
    qDebug() << "Options: " << options;
    return options;
}

void MainWindow::cleanup()
{
    QFileInfo lum(LUM);
    QFileInfo logfile("/tmp/" + lum.baseName() + ".log");
    if (logfile.exists())
        QFile::remove(logfile.absoluteFilePath());
    cmd.close();
}

// build the USB list
QStringList MainWindow::buildUsbList()
{
    QString drives
        = cmd.getCmdOut(QStringLiteral("lsblk --nodeps -nlo NAME,SIZE,MODEL,VENDOR -I 3,8,22,179,259"), true);
    return removeUnsuitable(drives.split(QStringLiteral("\n")));
}

// remove unsuitable drives from the list (live and unremovable)
QStringList MainWindow::removeUnsuitable(const QStringList &devices)
{
    QStringList list;
    QString name;
    for (const QString &line : devices) {
        name = line.split(QStringLiteral(" ")).at(0);
        if (ui->checkForceUsb->isChecked() || cmd.run(cli_utils + "is_usb_or_removable " + name.toUtf8(), true))
            if (cmd.getCmdOut(cli_utils + "get_drive $(get_live_dev) ", true) != name)
                list << line;
    }
    return list;
}

void MainWindow::cmdStart() { }

void MainWindow::cmdDone()
{
    timer.stop();
    ui->progBar->setValue(ui->progBar->maximum());
    setCursor(QCursor(Qt::ArrowCursor));
    ui->pushBack->show();
    if ((cmd.exitCode() == 0 && cmd.exitStatus() == QProcess::NormalExit) || ui->checkPretend->isChecked()) {
        QMessageBox::information(this, tr("Success"), tr("LiveUSB creation successful!"));
    } else {
        const QString mount_path = QStringLiteral("/run/live-usb-maker");
        if (QFile::exists(mount_path))
            QProcess::startDetached(QStringLiteral("umount"), {"-l", mount_path});
        QMessageBox::critical(this, tr("Failure"), tr("Error encountered in the LiveUSB creation process"));
    }
    cmd.disconnect();
}

// set proc and timer connections
void MainWindow::setConnections()
{
    timer.start(1s);
    connect(&cmd, &QProcess::readyRead, this, &MainWindow::updateOutput);
    connect(&cmd, &QProcess::started, this, &MainWindow::cmdStart);
    connect(&timer, &QTimer::timeout, this, &MainWindow::updateBar);
    connect(&cmd, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &MainWindow::cmdDone);
}

// set proper default mode based on iso contents
void MainWindow::setDefaultMode(const QString &iso_name)
{
    if (!isantiX_mx_family(iso_name)) {
        ui->radioDd->click();
        ui->radioNormal->setChecked(false);
    } else {
        ui->radioDd->setChecked(false);
        ui->radioNormal->click();
    }
}

void MainWindow::updateBar()
{
    stat_file->open(QIODevice::ReadOnly);
    QString out = stat_file->readAll();
    quint64 current_io = out.section(QRegularExpression(QStringLiteral("\\s+")), 7, 7).toULongLong();
    ui->progBar->setValue(current_io);
    stat_file->close();
}

void MainWindow::updateOutput()
{
    // remove escape sequences that are not handled by code
    QString out = cmd.readAll();
    out.remove(QRegularExpression(u8R"(\[0m|\]0;|\|\|\[1000D|\[74C||\[\?25l|\[\?25h|\[0;36m|\[1;37m)"));
    ui->outputBox->moveCursor(QTextCursor::End);
    if (out.contains(QLatin1String("\r"))) {
        ui->outputBox->moveCursor(QTextCursor::Up, QTextCursor::KeepAnchor);
        ui->outputBox->moveCursor(QTextCursor::EndOfLine, QTextCursor::KeepAnchor);
    }
    ui->outputBox->insertPlainText(out);

    QScrollBar *sb = ui->outputBox->verticalScrollBar();
    sb->setValue(sb->maximum());
    QApplication::processEvents();
}

void MainWindow::pushNext_clicked()
{
    // on first page
    if (ui->stackedWidget->currentIndex() == 0) {
        if (ui->comboUsb->currentText().isEmpty()) {
            QMessageBox::critical(this, tr("Error"), tr("Please select a USB device to write to"));
            return;
        }
        QString msg = tr("These actions will destroy all data on \n\n") + ui->comboUsb->currentText().simplified()
                      + "\n\n " + tr("Do you wish to continue?");
        if (QMessageBox::Yes != QMessageBox::warning(this, windowTitle(), msg, QMessageBox::Yes, QMessageBox::No))
            return;
        // pop the selection box if no valid selection (or clone)
        if (!(QFileInfo::exists(ui->pushSelectSource->property("filename").toString())
              || ui->pushSelectSource->property("filename").toString() == "clone")) {
            emit ui->pushSelectSource->clicked();
            return;
        }
        if (cmd.state() != QProcess::NotRunning) {
            ui->stackedWidget->setCurrentWidget(ui->outputPage);
            return;
        }
        ui->pushNext->setEnabled(false);
        ui->stackedWidget->setCurrentWidget(ui->outputPage);
        makeUsb(buildOptionList());
    }
}

void MainWindow::pushBack_clicked()
{
    this->setWindowTitle(QStringLiteral("Custom_Program_Name"));
    ui->stackedWidget->setCurrentIndex(0);
    ui->pushNext->setEnabled(true);
    ui->pushBack->hide();
    ui->outputBox->clear();
    ui->progBar->setValue(0);
}

void MainWindow::pushAbout_clicked()
{
    this->hide();
    displayAboutMsgBox(
        tr("About %1").arg(this->windowTitle()),
        "<p align=\"center\"><b><h2>" + this->windowTitle() + "</h2></b></p><p align=\"center\">" + tr("Version: ")
            + QApplication::applicationVersion() + "</p><p align=\"center\"><h3>"
            + tr("Program for creating a live-usb from an iso-file, another live-usb, a live-cd/dvd, or a running live "
                 "system.")
            + R"(</h3></p><p align="center"><a href="http://mxlinux.org">http://mxlinux.org</a><br /></p><p align="center">)"
            + tr("Copyright (c) MX Linux") + "<br /><br /></p>",
        QStringLiteral("/usr/share/doc/CUSTOMPROGRAMNAME/license.html"), tr("%1 License").arg(this->windowTitle()));
    this->show();
}

void MainWindow::pushHelp_clicked()
{
    QString url = QStringLiteral("/usr/share/doc/CUSTOMPROGRAMNAME/CUSTOMPROGRAMNAME.html");
    displayDoc(url, tr("%1 Help").arg(this->windowTitle()));
}

void MainWindow::pushSelectSource_clicked()
{
    const QString user = cmd.getCmdOut(QStringLiteral("logname"), true);
    const QString home = "/home/" + user;
    if (QString selected; !ui->checkCloneLive->isChecked() && !ui->checkCloneMode->isChecked()) {
        selected = QFileDialog::getOpenFileName(this, tr("Select an ISO file to write to the USB drive"), home,
                                                tr("ISO Files (*.iso);;All Files (*.*)"));
        if (!selected.isEmpty()) {
            ui->pushSelectSource->setText(selected);
            ui->pushSelectSource->setProperty("filename", selected);
            ui->pushSelectSource->setToolTip(selected);
            ui->pushSelectSource->setIcon(QIcon::fromTheme(QStringLiteral("media-cdrom")));
            ui->pushSelectSource->setStyleSheet(QStringLiteral("text-align: left;"));
            setDefaultMode(selected); // set proper default mode based on iso contents
        }
    } else if (ui->checkCloneMode->isChecked()) {
        selected = QFileDialog::getExistingDirectory(this, tr("Select Source Directory"), QString(QDir::rootPath()),
                                                     QFileDialog::ShowDirsOnly);
        if (QFileInfo::exists(selected + "/antiX/linuxfs") || QFileInfo::exists(selected + "/linuxfs")) {
            ui->pushSelectSource->setText(selected);
            ui->pushSelectSource->setProperty("filename", selected);
        } else {
            selected = (selected == QLatin1String("/")) ? QLatin1String("") : selected;
            QMessageBox::critical(this, tr("Failure"),
                                  tr("Could not find %1/antiX/linuxfs file")
                                      .arg(selected)); // TODO(adrian): -- the file might be in %/linuxfs too for frugal
        }
    }
}

void MainWindow::pushRefresh_clicked()
{
    ui->comboUsb->clear();
    ui->comboUsb->addItems(buildUsbList());
}

void MainWindow::pushOptions_clicked()
{
    if (advancedOptions) {
        ui->pushOptions->setText(tr("Show advanced options"));
        ui->groupAdvOptions->hide();
        advancedOptions = false;
        ui->pushOptions->setIcon(QIcon::fromTheme(QStringLiteral("go-down")));
        this->setMaximumHeight(height);
    } else {
        ui->pushOptions->setText(tr("Hide advanced options"));
        ui->groupAdvOptions->show();
        advancedOptions = true;
        ui->pushOptions->setIcon(QIcon::fromTheme(QStringLiteral("go-up")));
    }
}

void MainWindow::textLabel_textChanged(QString arg1)
{
    ui->textLabel->setText(arg1.remove(QStringLiteral(" ")));
    ui->textLabel->setCursorPosition(arg1.length());
}

void MainWindow::checkUpdate_clicked(bool checked)
{
    if (checked) {
        ui->checkSaveBoot->setEnabled(true);
    } else {
        ui->checkSaveBoot->setChecked(false);
        ui->checkSaveBoot->setEnabled(false);
    }
}

void MainWindow::checkCloneMode_clicked(bool checked)
{
    if (checked) {
        ui->pushSelectSource->setStyleSheet(QStringLiteral("text-align: center;"));
        ui->checkCloneLive->setChecked(false);
        checkCloneLive_clicked(false);
        ui->label_3->setText("<b>" + tr("Select Source") + "</b>");
        ui->pushSelectSource->setText(tr("Select Source Directory"));
        ui->pushSelectSource->setIcon(QIcon::fromTheme(QStringLiteral("folder")));
    } else {
        ui->label_3->setText("<b>" + tr("Select ISO file") + "</b>");
        ui->pushSelectSource->setText(tr("Select ISO"));
        ui->pushSelectSource->setIcon(QIcon::fromTheme(QStringLiteral("user-home")));
        ui->checkCloneLive->setEnabled(isRunningLive());
    }
    ui->pushSelectSource->setProperty("filename", "");
}

void MainWindow::checkCloneLive_clicked(bool checked)
{
    if (checked) {
        ui->pushSelectSource->setStyleSheet(QStringLiteral("text-align: center;"));
        ui->checkCloneMode->setChecked(false);
        ui->label_3->setText("<b>" + tr("Select Source") + "</b>");
        ui->pushSelectSource->setEnabled(false);
        ui->pushSelectSource->setText(tr("clone"));
        ui->pushSelectSource->setProperty("filename", "clone");
        ui->pushSelectSource->setIcon(QIcon::fromTheme(QStringLiteral("tools-media-optical-copy")));
        ui->pushSelectSource->blockSignals(true);
    } else {
        ui->label_3->setText("<b>" + tr("Select ISO file") + "</b>");
        ui->pushSelectSource->setEnabled(true);
        ui->pushSelectSource->setText(tr("Select ISO"));
        ui->pushSelectSource->setProperty("filename", "");
        ui->pushSelectSource->setIcon(QIcon::fromTheme(QStringLiteral("user-home")));
        ui->pushSelectSource->blockSignals(false);
    }
}

void MainWindow::radioDd_clicked()
{
    ui->radioNormal->setChecked(false);
    ui->checkCloneLive->setChecked(false);
    ui->checkCloneMode->setChecked(false);
    ui->checkEncrypt->setChecked(false);
    ui->checkCloneLive->setEnabled(false);
    ui->checkCloneMode->setEnabled(false);
    ui->checkEncrypt->setEnabled(false);
    ui->checkPretend->setEnabled(false);
    if (ui->groupAdvOptions->isVisible())
        pushOptions_clicked();
    ui->pushOptions->setEnabled(false);
    ui->label_percent->setEnabled(false);
    ui->label_part_label->setEnabled(false);
    ui->spinBoxSize->setEnabled(false);
    ui->textLabel->setEnabled(false);
}

void MainWindow::radioNormal_clicked()
{
    ui->checkCloneLive->setEnabled(isRunningLive());
    ui->checkCloneMode->setEnabled(true);
    ui->checkEncrypt->setEnabled(true);
    ui->checkPretend->setEnabled(true);
    ui->pushOptions->setEnabled(true);
    ui->label_percent->setEnabled(true);
    ui->label_part_label->setEnabled(true);
    ui->spinBoxSize->setEnabled(true);
    ui->textLabel->setEnabled(true);
}

bool MainWindow::isantiX_mx_family(const QString &selected)
{
    Cmd cmd;
    return cmd.run(
        QStringLiteral("xorriso -indev '%1' -find /antiX -name linuxfs -prune  2>/dev/null | grep -q /antiX/linuxfs")
            .arg(selected),
        true);
}

void MainWindow::pushLumLogFile_clicked()
{
    QFileInfo lum(LUM);
    QString url = "file:///tmp/" + lum.baseName() + ".log";
    qDebug() << "lumlog" << url;
    if (!QFileInfo::exists("/var/log/" + lum.baseName() + ".log")) {
        QMessageBox::information(this, QApplication::applicationName(),
                                 tr("Could not find a log file at: ") + "/var/log/" + lum.baseName() + ".log");
        return;
    }
    QFileInfo viewer(QStringLiteral("/usr/bin/mx-viewer"));
    QFileInfo viewer2(QStringLiteral("/usr/bin/antix-viewer"));
    QString rootrunoption = QString();

    // generate temporary log file
    QString cmd_str
        = "tac /var/log/" + lum.baseName() + R"(.log | sed "/^=\{60\}=*$/q" |tac > /tmp/)" + lum.baseName() + ".log ";
    Cmd cmd; // new Cmd so it allows user opening the log while the ISO is being burned
    cmd.run(cmd_str);

    if (getuid() == 0)
        rootrunoption = QStringLiteral("runuser $(logname) -c ");

    if (viewer.exists()) {
        qputenv("HOME", starting_home.toUtf8());
        cmd_str = QStringLiteral("mx-viewer %1 '%2' &").arg(url, lum.baseName());
        qputenv("HOME", "/root");
    } else if (viewer2.exists()) {
        cmd_str = QStringLiteral("antix-viewer %1 '%2' &").arg(url, lum.baseName());
    } else {
        cmd_str = QString(rootrunoption + "\"DISPLAY=$DISPLAY xdg-open %1\" &").arg(url);
    }
    cmd.run(cmd_str);
}

void MainWindow::spinBoxSize_valueChanged(int arg1)
{
    const int max = ui->spinBoxSize->maximum();
    ui->checkDataFirst->setEnabled(arg1 == max);
    ui->spinBoxDataSize->setEnabled(arg1 == max);
    ui->comboBoxDataFormat->setEnabled(arg1 == max);
    ui->labelFormat->setEnabled(arg1 == max);
}

void MainWindow::checkDataFirst_clicked(bool checked) { ui->spinBoxSize->setDisabled(checked); }
