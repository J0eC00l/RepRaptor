#include "mainwindow.h"
#include "ui_mainwindow.h"

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    //Setup the UI
    ui->fileBox->setDisabled(true);
    ui->sendBtn->setDisabled(true);
    ui->pauseBtn->setDisabled(true);
    ui->progressBar->setValue(0);
    ui->controlBox->setDisabled(true);
    ui->consoleGroup->setDisabled(true);
    ui->pauseBtn->setDisabled(true);
    ui->actionPrint_from_SD->setDisabled(true);
    ui->actionSet_SD_printing_mode->setDisabled(true);
    ui->actionEEPROM_editor->setDisabled(true);
    ui->statusGroup->setDisabled(true);
    ui->extruderlcd->setPalette(Qt::red);
    ui->bedlcd->setPalette(Qt::red);
    ui->sendtext->installEventFilter(this);
    ui->etmpspin->installEventFilter(this);
    ui->btmpspin->installEventFilter(this);
    recentMenu = new QMenu(this);
    recentMenu->setTitle("Recent files");
    ui->menuFile->insertMenu(ui->actionSettings, recentMenu);
    ui->menuFile->insertSeparator(ui->actionSettings);

    //Init baudrate combobox
    ui->baudbox->addItem(QString::number(4800));
    ui->baudbox->addItem(QString::number(9600));
    ui->baudbox->addItem(QString::number(115200));
    ui->baudbox->addItem(QString::number(128000));
    ui->baudbox->addItem(QString::number(230400));
    ui->baudbox->addItem(QString::number(250000));
    ui->baudbox->addItem(QString::number(460800));
    ui->baudbox->addItem(QString::number(500000));

    //Create objects
    //Timers need parent to be set up
    //in order to be tied to its thread
    //and stop right
    statusTimer = new QTimer(this);
    progressSDTimer = new QTimer(this);
    //QElapsedTimers have no parents
    sinceLastTemp = new QElapsedTimer();
    sinceLastSDStatus = new QElapsedTimer();
    //Workers would be moved to ther thread,
    //so we should not set their parents. Instead,
    //we should connect QThread's finished() to
    //worker's deleteLater()
    parserWorker = new Parser();
    senderWorker = new Sender();
    //Setting the parent of QThread lets
    //threads to stop right, and also makes
    //them inherit parent's priority
    parserThread = new QThread(this);
    senderThread = new QThread(this);

    //Restore settings
    firstrun = !settings.value("core/firstrun").toBool(); //firstrun is inverted!
    ui->baudbox->setCurrentIndex(settings.value("printer/baudrateindex", 2).toInt());
    checkingTemperature = settings.value("core/checktemperature", 0).toBool();
    ui->checktemp->setChecked(checkingTemperature);
    ui->etmpspin->setValue(settings.value("user/extrudertemp", 210).toInt());
    ui->btmpspin->setValue(settings.value("user/bedtemp", 60).toInt());
    echo = settings.value("core/echo", 0).toBool();
    autolock = settings.value("core/lockcontrols", 0).toBool();
    chekingSDStatus = settings.value("core/checksdstatus", 1).toBool();
    firmware = settings.value("printer/firmware", OtherFirmware).toInt();
    statusTimer->setInterval(settings.value("core/statusinterval", 3000).toInt());
    int size = settings.beginReadArray("user/recentfiles");
    for(int i = 0; i < size; ++i)
    {
        settings.setArrayIndex(i);
        recentFiles.append(settings.value("user/file").toString());
    }
    settings.endArray();

    //Init values
    sending = false;
    paused = false;
    sdprinting = false;
    opened = false;
    sdBytes = 0;
    userHistoryPos = 0;
    userHistory.append("");

    //Update serial ports
    serialupdate();

    //Internal signal-slots
    connect(statusTimer, SIGNAL(timeout()), this, SLOT(checkStatus()));
    connect(progressSDTimer, SIGNAL(timeout()), this, SLOT(checkSDStatus()));
    connect(this, SIGNAL(eepromReady()), this, SLOT(openEEPROMeditor()));

    //Register all the types
    qRegisterMetaType<TemperatureReadings>("TemperatureReadings");
    qRegisterMetaType<SDProgress>("SDProgress");
    qRegisterMetaType<QSerialPortInfo>("QSerialPortInfo");
    qRegisterMetaType< QVector<QString> >("QVector<QString>");
    qRegisterMetaType<FileProgress>("FileProgress");
    qRegisterMetaType<QSerialPort::SerialPortError>("QSerialPort::SerialPortError");

    //Parser thread signal-slots and init
    parserWorker->moveToThread(parserThread);
    connect(parserThread, &QThread::finished, parserWorker, &QObject::deleteLater);
    connect(this, &MainWindow::receivedData, parserWorker, &Parser::parse);
    connect(this, &MainWindow::startedReadingEEPROM, parserWorker, &Parser::setEEPROMReadingMode);
    connect(parserWorker, &Parser::receivedTemperature, this, &MainWindow::updateTemperature);
    connect(parserWorker, &Parser::receivedSDFilesList, this, &MainWindow::initSDprinting);
    connect(parserWorker, &Parser::receivedEEPROMLine, this, &MainWindow::EEPROMSettingReceived);
    connect(parserWorker, &Parser::recievingEEPROMDone, this, &MainWindow::openEEPROMeditor);
    connect(parserWorker, &Parser::receivedError, this, &MainWindow::receivedError);
    connect(parserWorker, &Parser::receivedSDDone, this, &MainWindow::receivedSDDone);
    connect(parserWorker, &Parser::receivedSDUpdate, this, &MainWindow::updateSDStatus);
    parserThread->start();

    //Sender thread signal-slots and init
    senderWorker->moveToThread(senderThread);
    connect(senderThread, &QThread::finished, senderWorker, &QObject::deleteLater);
    connect(parserWorker, &Parser::receivedOkNum, senderWorker, &Sender::receivedOkNum);
    connect(parserWorker, &Parser::receivedOkWait, senderWorker, &Sender::receivedOkWait);
    connect(parserWorker, &Parser::receivedResend, senderWorker, &Sender::receivedResend);
    connect(parserWorker, &Parser::receivedStart, senderWorker, &Sender::receivedStart);
    connect(senderWorker, &Sender::errorReceived, this, &MainWindow::serialError);
    connect(senderWorker, &Sender::dataReceived, parserWorker, &Parser::parse, Qt::QueuedConnection);
    connect(senderWorker, &Sender::dataReceived, this, &MainWindow::readSerial, Qt::QueuedConnection);
    connect(senderWorker, &Sender::reportProgress, this, &MainWindow::updateFileProgress);
    connect(senderWorker, &Sender::baudrateSetFailed, this, &MainWindow::baudrateSetFailed);
    connect(this, &MainWindow::setFile, senderWorker, &Sender::setFile);
    connect(this, &MainWindow::startPrinting, senderWorker, &Sender::startPrinting);
    connect(this, &MainWindow::stopPrinting, senderWorker, &Sender::stopPrinting);
    connect(this, &MainWindow::pause, senderWorker, &Sender::pause);
    connect(this, &MainWindow::setBaudrate, senderWorker, &Sender::setBaudrate);
    connect(this, &MainWindow::openPort, senderWorker, &Sender::openPort);
    connect(this, &MainWindow::closePort, senderWorker, &Sender::closePort);
    connect(this, &MainWindow::injectCommand, senderWorker, &Sender::injectCommand);
    connect(this, &MainWindow::flushInjectionBuffer, senderWorker, &Sender::flushInjectionBuffer);
    senderThread->start();

    //Timers init
    statusTimer->start();
    progressSDTimer->setInterval(2500);
    if(chekingSDStatus) progressSDTimer->start();
    sinceLastTemp->start();
    sinceLastSDStatus->start();

    //Update recent files list
    updateRecent();
}

MainWindow::~MainWindow()
{
    //Save settings
    if(firstrun) settings.setValue("core/firstrun", true); //firstrun is inverted!
    settings.setValue("printer/baudrateindex", ui->baudbox->currentIndex());
    settings.setValue("core/checktemperature", ui->checktemp->isChecked());
    settings.setValue("user/extrudertemp", ui->etmpspin->value());
    settings.setValue("user/bedtemp", ui->btmpspin->value());
    settings.beginWriteArray("user/recentfiles");
    for(int i = 0; i < recentFiles.size(); ++i)
    {
        settings.setArrayIndex(i);
        settings.setValue("user/file", recentFiles.at(i));
    }
    settings.endArray();

    //Cleanup what is left
    if(gfile.isOpen()) gfile.close();
    statusTimer->stop();
    progressSDTimer->stop();
    parserThread->quit();
    parserThread->wait();
    senderThread->quit();
    senderThread->wait();

    delete ui;
}

void MainWindow::open()
{
    sdprinting = false;
    QString filename;
    QDir home;
    filename = QFileDialog::getOpenFileName(this,
                                            "Open GCODE",
                                            home.home().absolutePath(),
                                            "GCODE (*.g *.gco *.gcode *.nc)");
    gfile.setFileName(filename);
    if(!recentFiles.contains(filename))
    {
        recentFiles.prepend(filename);
        if(recentFiles.size() >= 10) recentFiles.removeLast();
    }

    updateRecent();
    parseFile(filename);
}

void MainWindow::parseFile(QString filename)
{
    gfile.setFileName(filename);
    gcode.clear();
    if (gfile.open(QIODevice::ReadOnly))
    {
        QTextStream in(&gfile);
        while (!in.atEnd())
        {
            QString line = in.readLine();
            if(!line.startsWith(";")) gcode.append(line);
        }
        gfile.close();
        emit setFile(gcode);
        ui->fileBox->setEnabled(true);
        ui->progressBar->setEnabled(true);
        ui->sendBtn->setText("Send");
        ui->filename->setText(gfile.fileName().split(QDir::separator()).last());
        ui->filelines->setText(QString::number(gcode.size()) + QString("/0 lines"));
    }
}

void MainWindow::serialupdate()
{
    //Clear old serialports
    ui->serialBox->clear();
    //Save ports list
    QList<QSerialPortInfo> list = QSerialPortInfo::availablePorts();
    //Populate the combobox
    for(int i = 0; i < list.size(); i++)
        ui->serialBox->addItem(list.at(i).portName());
}

void MainWindow::serialconnect()
{
    //Hust to be safe - wipe all the user commands
    emit flushInjectionBuffer();

    if(!opened)
    {
        //Find the portinfo we need
        foreach (const QSerialPortInfo &info, QSerialPortInfo::availablePorts())
        {
            if(info.portName() == ui->serialBox->currentText())
            {
                printerinfo = info;
                break;
            }
        }

        //Emit signals needed
        emit setBaudrate(ui->baudbox->currentText().toInt());
        emit openPort(printerinfo);
        //Set the flag right
        opened=true;
        //Update UI
        ui->connectBtn->setText("Disconnect");
        ui->sendBtn->setDisabled(false);
        ui->progressBar->setValue(0);
        ui->controlBox->setDisabled(false);
        ui->consoleGroup->setDisabled(false);
        ui->statusGroup->setDisabled(false);
        ui->actionPrint_from_SD->setEnabled(true);
        ui->actionSet_SD_printing_mode->setEnabled(true);
        if(firmware == Repetier) ui->actionEEPROM_editor->setDisabled(false);
    }

    else if(opened)
    {
        //Emit closing signal to Sender
        emit closePort();
        //Set the flag
        opened = false;
        //Update UI
        ui->connectBtn->setText("Connect");
        ui->sendBtn->setDisabled(true);
        ui->pauseBtn->setDisabled(true);
        ui->progressBar->setValue(0);
        ui->controlBox->setDisabled(true);
        ui->consoleGroup->setDisabled(true);
        ui->actionPrint_from_SD->setDisabled(true);
        ui->actionSet_SD_printing_mode->setDisabled(true);
        ui->statusGroup->setDisabled(true);
        ui->actionEEPROM_editor->setDisabled(false);
     }

}

/////////////////
//Buttons start//
/////////////////
void MainWindow::xplus()
{
    QString command = "G91\nG1 X" + ui->stepspin->text() + "\nG90";
    emit injectCommand(command);
}

void MainWindow::xminus()
{
    QString command = "G91\nG1 X-" + ui->stepspin->text() + "\nG90";
    emit injectCommand(command);
}

void MainWindow::xhome()
{
    emit injectCommand("G28 X0");
}

void MainWindow::yplus()
{
    QString command = "G91\nG1 Y" + ui->stepspin->text() + "\nG90";
    emit injectCommand(command);
}

void MainWindow::yminus()
{
    QString command = "G91\nG1 Y-" + ui->stepspin->text() + "\nG90";
    emit injectCommand(command);
}

void MainWindow::yhome()
{
    emit injectCommand("G28 Y0");
}

void MainWindow::zplus()
{
    QString command = "G91\nG1 Z" + ui->stepspin->text() + "\nG90";
    emit injectCommand(command);
}

void MainWindow::zminus()
{
    QString command = "G91\nG1 Z-" + ui->stepspin->text() + "\nG90";
    emit injectCommand(command);
}

void MainWindow::zhome()
{
    emit injectCommand("G28 Z0");
}

void MainWindow::eplus()
{
    QString command = "G91\nG1 E" + ui->estepspin->text() + "\nG90";
    emit injectCommand(command);
}

void MainWindow::eminus()
{
    QString command = "G91\nG1 E-" + ui->estepspin->text() + "\nG90";
    emit injectCommand(command);
}

void MainWindow::ezero()
{
    emit injectCommand("G92 E0");
}

void MainWindow::homeall()
{
    emit injectCommand("G28");
}

void MainWindow::on_sendbtn_clicked()
{
    QString command = ui->sendtext->text();
    emit injectCommand(command);
    userHistory.append(command);
    userHistoryPos = 0;
}

void MainWindow::on_fanonbtn_clicked()
{
    emit injectCommand("M106");
}

void MainWindow::on_fanoffbtn_clicked()
{
    emit injectCommand("M107");
}

void MainWindow::on_atxonbtn_clicked()
{
    emit injectCommand("M80");
}

void MainWindow::on_atxoffbtn_clicked()
{
    emit injectCommand("M81");
}

void MainWindow::on_etmpset_clicked()
{
    QString command = "M80\nM104 S" + ui->etmpspin->text();
    emit injectCommand(command);
}

void MainWindow::on_etmpoff_clicked()
{
    emit injectCommand("M104 S0");
}

void MainWindow::on_btmpset_clicked()
{
    QString command = "M80\nM140 S" + ui->btmpspin->text();
    emit injectCommand(command);
}

void MainWindow::on_btmpoff_clicked()
{
    emit injectCommand("M140 S0");
}

void MainWindow::bedcenter()
{
    int x, y;

    x = settings.value("printer/bedx", 200).toInt();
    y = settings.value("printer/bedy", 200).toInt();

    QString command = "G1 X" + QString::number(x/2) + "Y" + QString::number(y/2);
    emit injectCommand(command);
}

void MainWindow::on_speedslider_valueChanged(int value)
{
    ui->speededit->setText(QString::number(value));
}

void MainWindow::on_speededit_textChanged(const QString &arg1)
{
    if(arg1.toInt()) ui->speedslider->setValue(arg1.toInt());
    else ui->speededit->setText(QString::number(ui->speedslider->value()));
}

void MainWindow::on_speedsetbtn_clicked()
{
    QString command = "M220 S" + QString::number(ui->speedslider->value());
    emit injectCommand(command);
}

void MainWindow::on_flowedit_textChanged(const QString &arg1)
{
    if(arg1.toInt()) ui->flowslider->setValue(arg1.toInt());
    else ui->flowedit->setText(QString::number(ui->flowslider->value()));
}

void MainWindow::on_flowslider_valueChanged(int value)
{
    ui->flowedit->setText(QString::number(value));
}

void MainWindow::on_flowbutton_clicked()
{
    QString command = "M221 S" + QString::number(ui->flowslider->value());
    emit injectCommand(command);
}

void MainWindow::on_haltbtn_clicked()
{
    if(sending && !paused)ui->pauseBtn->click();
    emit flushInjectionBuffer();
    emit injectCommand("M112");
}

void MainWindow::on_actionPrint_from_SD_triggered()
{
    emit injectCommand("M20");
}

void MainWindow::on_sendBtn_clicked()
{
    emit flushInjectionBuffer();
    if(sending && !sdprinting)
    {
        sending = false;
        ui->sendBtn->setText("Send");
        ui->pauseBtn->setText("Pause");
        ui->pauseBtn->setDisabled(true);
        if(autolock) ui->controlBox->setChecked(true);
        paused = false;
        emit pause(paused);
        emit stopPrinting();
    }
    else if(!sending && !sdprinting)
    {
        sending=true;
        ui->sendBtn->setText("Stop");
        ui->pauseBtn->setText("Pause");
        ui->pauseBtn->setEnabled(true);
        if(autolock) ui->controlBox->setChecked(false);
        paused = false;
        emit pause(paused);
        emit startPrinting();
    }
    else if(sdprinting)
    {
        sending = false;
        emit injectCommand("M24");
        emit injectCommand("M27");
        ui->sendBtn->setText("Start");
        ui->pauseBtn->setText("Pause");
        ui->pauseBtn->setEnabled(true);
        if(autolock) ui->controlBox->setChecked(true);
        paused = false;
        emit pause(paused);
    }

    ui->progressBar->setValue(0);
}

void MainWindow::on_pauseBtn_clicked()
{
    if(paused && !sdprinting)
    {
        paused = false;
        emit pause(paused);
        if(autolock) ui->controlBox->setChecked(false);
        ui->pauseBtn->setText("Pause");
    }
    else if(!paused && !sdprinting)
    {
        paused = true;
        emit pause(paused);
        if(autolock) ui->controlBox->setChecked(true);
        ui->pauseBtn->setText("Resume");
    }
    else if(sdprinting)
    {
        emit injectCommand("M25");
    }
}

void MainWindow::on_releasebtn_clicked()
{
    emit injectCommand("M84");
}

void MainWindow::on_actionSettings_triggered()
{
    SettingsWindow settingswindow(this);

    settingswindow.exec();
}

void MainWindow::on_actionAbout_triggered()
{
    AboutWindow aboutwindow(this);

    aboutwindow.exec();
}

void MainWindow::on_actionAbout_Qt_triggered()
{
    qApp->aboutQt();
}
/////////////////
//Buttons end  //
/////////////////

void MainWindow::readSerial(QByteArray data)
{
    printMsg(QString(data)); //echo
}

void MainWindow::printMsg(QString text)
{
    //Get the cursor and set it to the end
    QTextCursor cursor = ui->terminal->textCursor();
    cursor.movePosition(QTextCursor::End);

    //Paste the text
    cursor.insertText(text);

    //Apply
    ui->terminal->setTextCursor(cursor);
}

void MainWindow::checkStatus()
{
    //We want to check for few things here:
    //if we are checking temperature at all and
    //if the time passed from the time we last
    //received update is more than the check
    //interval
    if(checkingTemperature
            &&(sinceLastTemp->elapsed() > statusTimer->interval())) emit injectCommand("M105");
}

void MainWindow::on_checktemp_stateChanged(int arg1)
{
    if(arg1) checkingTemperature = true;
    else checkingTemperature = false;
}

void MainWindow::updateRecent()
{
    if(!recentFiles.isEmpty()) //If array is empty we should not be bothered
    {
        recentMenu->clear(); //Clear menu from previos actions
        foreach (QString str, recentFiles)
        {
            if (!str.isEmpty()) //Empty strings are not needed
            {
                QAction *action = new QAction(this);
                action->setText(str); //Set filepath as a title
                action->setObjectName(str); //Also set name to the path so we can get it later
                recentMenu->addAction(action); //Add action to the menu
                connect(action, SIGNAL(triggered()), this, SLOT(recentClicked()));
            }
        }
    }
}

void MainWindow::serialError(QSerialPort::SerialPortError error)
{
    if(error == QSerialPort::NoError) return;
    if(error == QSerialPort::NotOpenError) return; //this error is internal

    emit closePort(); //If the port is still open we need to close it

    //Set RepRaptor to pause if printing
    //We dont want to stop the printing,
    //in order to be able to continue it
    //after possible connection fail
    if(sending) paused = true;
    emit pause(paused);

    //Flush all the user commands
    emit flushInjectionBuffer();

    //Set the flag
    opened = false;

    //Update UI
    ui->connectBtn->setText("Connect");
    ui->sendBtn->setDisabled(true);
    ui->pauseBtn->setDisabled(true);
    ui->controlBox->setDisabled(true);
    ui->consoleGroup->setDisabled(true);
    ui->actionPrint_from_SD->setDisabled(true);
    ui->actionSet_SD_printing_mode->setDisabled(true);
    ui->actionEEPROM_editor->setDisabled(true);

    //Print error to qDebug to see it in terminal
    qDebug() << error;

    //Identify error
    QString errorMsg;
    switch(error)
    {
    case QSerialPort::DeviceNotFoundError:
        errorMsg = "Device not found";
        break;

    case QSerialPort::PermissionError:
        errorMsg = "Insufficient permissions\nAlready opened?";
        break;

    case QSerialPort::OpenError:
        errorMsg = "Cant open port\nAlready opened?";
        break;

    case QSerialPort::TimeoutError:
        errorMsg = "Serial connection timed out";
        break;

    //These errors are the same really
    case QSerialPort::WriteError:
    case QSerialPort::ReadError:
        errorMsg = "I/O Error";
        break;

    case QSerialPort::ResourceError:
        errorMsg = "Disconnected";
        break;

    default:
        errorMsg = "Unknown error\nSomething went wrong";
        break;
    }

    //Spawn the error message
    ErrorWindow errorwindow(this, errorMsg);
    errorwindow.exec();
}

void MainWindow::updateTemperature(TemperatureReadings r)
{
    ui->extruderlcd->display(r.e);
    ui->bedlcd->display(r.b);
    ui->tempLine->setText(r.raw);
    sinceLastTemp->restart();
}

void MainWindow::initSDprinting(QStringList sdFiles)
{
    SDWindow sdwindow(sdFiles, this);

    connect(&sdwindow, SIGNAL(fileSelected(QString)), this, SLOT(selectSDfile(QString)));

    sdwindow.exec();
}


void MainWindow::selectSDfile(QString file)
{
    QStringList split = file.split(' ');
    QString bytes = split.at(split.size()-1);
    QString filename = file.remove(" "+bytes);

    ui->filename->setText(filename);
    if(chekingSDStatus)
    {
        ui->filelines->setText(bytes + QString("/0 bytes"));
        ui->progressBar->setEnabled(true);
        ui->progressBar->setValue(0);
    }
    else ui->progressBar->setDisabled(true);
    ui->sendBtn->setText("Start");
    sdBytes = bytes.toDouble();

    emit flushInjectionBuffer();
    emit injectCommand("M23 " + filename);
    sdprinting = true;
    ui->fileBox->setDisabled(false);
}

void MainWindow::updateSDStatus(SDProgress p)
{
    ui->filelines->setText(QString::number(p.progress)
                           + QString("/")
                           + QString::number(p.total)
                           + QString(" bytes"));
    if(p.progress != 0) ui->progressBar->setValue(((double)p.progress/p.total) * 100);
    else ui->progressBar->setValue(0);
    if(p.total == p.progress) sdprinting = false;
    sinceLastSDStatus->restart();
}

void MainWindow::checkSDStatus()
{
    //Same as temperature check
    if(sdprinting && chekingSDStatus && sinceLastSDStatus->elapsed() > progressSDTimer->interval())
        emit injectCommand("M27");
}

void MainWindow::on_stepspin_valueChanged(const QString &arg1)
{
    if(arg1.toFloat() < 1) ui->stepspin->setSingleStep(0.1);
    else if(arg1.toFloat() >=10) ui->stepspin->setSingleStep(10);
    else if(arg1.toFloat() >= 1) ui->stepspin->setSingleStep(1);
}

void MainWindow::on_estepspin_valueChanged(const QString &arg1)
{
    if(arg1.toFloat() < 1) ui->estepspin->setSingleStep(0.1);
    else if(arg1.toFloat() >=10) ui->estepspin->setSingleStep(10);
    else if(arg1.toFloat() >= 1) ui->estepspin->setSingleStep(1);
}

void MainWindow::on_actionSet_SD_printing_mode_triggered()
{
    //This is a manual switch to SD printing mode
    //It can be useful if you have no display, and connected
    //to a printer already printing something
    sdprinting = true;
    ui->fileBox->setDisabled(false);
}

void MainWindow::requestEEPROMSettings()
{
    emit flushInjectionBuffer(); //Request ASAP
    EEPROMSettings.clear(); //Clear anything stored before
    //Find out what to call according to firmware
    switch(firmware)
    {
    case Marlin:
        emit injectCommand("M503");
        break;
    case Repetier:
        emit injectCommand("M205");
        break;
    case OtherFirmware:
        return;
    }

   emit startedReadingEEPROM();
}

void MainWindow::on_actionEEPROM_editor_triggered()
{
    requestEEPROMSettings();
}

void MainWindow::openEEPROMeditor()
{
    EEPROMWindow eepromwindow(EEPROMSettings, this);

    eepromwindow.setWindowModality(Qt::NonModal); //Do not bloct the UI when EEPROM editor is shown
    connect(&eepromwindow, SIGNAL(changesComplete(QStringList)), this, SLOT(sendEEPROMsettings(QStringList)));

    eepromwindow.exec();
}

void MainWindow::sendEEPROMsettings(QStringList changes)
{
    emit flushInjectionBuffer(); //To send EEPROM settings asap
    foreach (QString str, changes)
    {
        emit injectCommand(str);
    }
}

void MainWindow::EEPROMSettingReceived(QString esetting)
{
    EEPROMSettings.append(esetting);
}

void MainWindow::receivedError()
{
    //This should be raised if "!!" received
    ErrorWindow errorwindow(this,"Hardware failure");
    errorwindow.exec();
}

void MainWindow::receivedSDDone()
{
    sdprinting=false;
    ui->progressBar->setValue(0);
    ui->filename->setText("");
    ui->fileBox->setDisabled(true);
}

void MainWindow::updateFileProgress(FileProgress p)
{
    //Check if file is at end
    if(p.P >= p.T)
    {
        ui->sendBtn->setText("Send");
        ui->pauseBtn->setDisabled(true);
        sending = false;
        paused = false;
        emit pause(paused);
    }
    else
    {
        ui->sendBtn->setText("Stop");
        ui->pauseBtn->setEnabled(true);
        sending = true;
    }
    //Update progressbar and text lines
    ui->filelines->setText(QString::number(p.T)
                        + QString("/")
                        + QString::number(p.P)
                        + QString(" Lines"));
    ui->progressBar->setValue(((float)p.P/p.T) * 100);
}

void MainWindow::baudrateSetFailed(int b)
{
    ErrorWindow errorwindow(this, QString("Baudrate set failed:\n" +
                                  QString::number(b) +
                                  " baud"));
    errorwindow.show();
}

//Needed for keypress handling
bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    //User history
    if(obj == ui->sendtext && !userHistory.isEmpty())
    {
        if(event->type() == QEvent::KeyPress)
        {
            QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event); //Fetch the event

            if (keyEvent->key() == Qt::Key_Up) //Scroll up with up arrow
            {
                if(++userHistoryPos <= userHistory.size()-1)
                    ui->sendtext->setText(userHistory.at(userHistoryPos));
                else userHistoryPos--;
                return true;
            }
            else if(keyEvent->key() == Qt::Key_Down) //Scroll down with down arow
            {
                if(--userHistoryPos >= 0)
                    ui->sendtext->setText(userHistory.at(userHistoryPos));
                else userHistoryPos++;
                return true;
            }
        }
        return false;
    }
    //Temperature
    else if(obj == ui->etmpspin)
    {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);

        if(keyEvent->key() == Qt::Key_Return)
        {
            ui->etmpset->click(); //Emulate keypress
            return true;
        }
        return false;
    }
    else if(obj == ui->btmpspin)
    {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);

        if(keyEvent->key() == Qt::Key_Return)
        {
            ui->btmpset->click(); //Emulate keypress
            return true;
        }
        return false;
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::recentClicked()
{
    //Actually a dirty hack, but it is fast and simple
    //So this slot is not for anything to trigger, but
    //recent files menu
    parseFile(sender()->objectName());
}
