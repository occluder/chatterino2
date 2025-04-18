#include "widgets/helper/ResizingTextEdit.hpp"

#include "common/Common.hpp"
#include "common/QLogging.hpp"
#include "controllers/completion/TabCompletionModel.hpp"
#include "singletons/Settings.hpp"

#include <QMimeData>
#include <QMimeDatabase>
#include <QObject>

namespace chatterino {

ResizingTextEdit::ResizingTextEdit()
{
    auto sizePolicy = this->sizePolicy();
    sizePolicy.setHeightForWidth(true);
    sizePolicy.setVerticalPolicy(QSizePolicy::Preferred);
    this->setSizePolicy(sizePolicy);
    this->setAcceptRichText(false);

    QObject::connect(this, &QTextEdit::textChanged, this,
                     &QWidget::updateGeometry);

    QObject::connect(this, &QTextEdit::cursorPositionChanged, [this]() {
        // If tab was pressed and we're completing/replacing the current word,
        // this code will not even be called, see ResizingTextEdit::keyPressEvent

        if (!this->completionInProgress_)
        {
            return;
        }
        qCDebug(chatterinoCommon)
            << "Finishing completion because cursor moved";
        this->completionInProgress_ = false;
    });

    // Whenever the setting for emote completion changes, force a
    // refresh on the completion model the next time "Tab" is pressed
    getSettings()->prefixOnlyEmoteCompletion.connect([this] {
        this->completionInProgress_ = false;
    });

    this->setFocusPolicy(Qt::ClickFocus);
    this->installEventFilter(this);
}

QSize ResizingTextEdit::sizeHint() const
{
    return QSize(this->width(), this->heightForWidth(this->width()));
}

bool ResizingTextEdit::hasHeightForWidth() const
{
    return true;
}

bool ResizingTextEdit::isFirstWord() const
{
    QString plainText = this->toPlainText();
    QString portionBeforeCursor = plainText.left(this->textCursor().position());
    return !portionBeforeCursor.contains(' ');
};

int ResizingTextEdit::heightForWidth(int) const
{
    auto margins = this->contentsMargins();

    return margins.top() + document()->size().height() + margins.bottom() + 5;
}

QString ResizingTextEdit::textUnderCursor(bool *hadSpace) const
{
    auto currentText = this->toPlainText();

    QTextCursor tc = this->textCursor();

    auto textUpToCursor = currentText.left(tc.selectionStart());

    auto words = QStringView{textUpToCursor}.split(' ');
    if (words.size() == 0)
    {
        return QString();
    }

    bool first = true;
    QString lastWord;
    for (auto it = words.crbegin(); it != words.crend(); ++it)
    {
        auto word = *it;

        if (first && word.isEmpty())
        {
            first = false;
            if (hadSpace != nullptr)
            {
                *hadSpace = true;
            }
            continue;
        }

        lastWord = word.toString();
        break;
    }

    if (lastWord.isEmpty())
    {
        return QString();
    }

    return lastWord;
}

bool ResizingTextEdit::eventFilter(QObject *obj, QEvent *event)
{
    (void)obj;  // unused

    // makes QShortcuts work in the ResizingTextEdit
    if (event->type() != QEvent::ShortcutOverride)
    {
        return false;
    }
    auto *ev = static_cast<QKeyEvent *>(event);
    ev->ignore();
    if ((ev->key() == Qt::Key_C || ev->key() == Qt::Key_Insert) &&
        ev->modifiers() == Qt::ControlModifier)
    {
        return false;
    }
    return true;
}
void ResizingTextEdit::keyPressEvent(QKeyEvent *event)
{
#ifdef Q_OS_MACOS
    if ((event->modifiers() == Qt::ControlModifier) &&
        (event->key() == Qt::Key_Backspace))
    {
        QTextCursor cursor = this->textCursor();
        cursor.movePosition(QTextCursor::StartOfLine, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
        this->setTextCursor(cursor);
        return;
    }
#endif

    event->ignore();

    this->keyPressed.invoke(event);

    bool doComplete =
        (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab) &&
        (event->modifiers() & Qt::ControlModifier) == Qt::NoModifier &&
        !event->isAccepted();

    if (doComplete)
    {
        // check if there is a completer
        if (!this->completer_)
        {
            return;
        }

        QString currentCompletion = this->textUnderCursor();

        // check if there is something to complete
        if (currentCompletion.size() <= 1)
        {
            return;
        }

        // always expected to be TabCompletionModel
        auto *completionModel =
            dynamic_cast<TabCompletionModel *>(this->completer_->model());
        assert(completionModel != nullptr);

        if (!this->completionInProgress_)
        {
            // First type pressing tab after modifying a message, we refresh our
            // completion model
            this->completer_->setModel(completionModel);
            completionModel->updateResults(
                currentCompletion, this->toPlainText(),
                this->textCursor().position(), this->isFirstWord());
            this->completionInProgress_ = true;
            {
                // this blocks cursor movement events from resetting tab completion
                QSignalBlocker dontTriggerCursorMovement(this);
                this->completer_->complete();
            }
            return;
        }

        // scrolling through selections
        if (event->key() == Qt::Key_Tab)
        {
            if (!this->completer_->setCurrentRow(
                    this->completer_->currentRow() + 1))
            {
                // wrap over and start again
                this->completer_->setCurrentRow(0);
            }
        }
        else
        {
            if (!this->completer_->setCurrentRow(
                    this->completer_->currentRow() - 1))
            {
                // wrap over and start again
                this->completer_->setCurrentRow(
                    this->completer_->completionCount() - 1);
            }
        }

        {
            // this blocks cursor movement events from updating tab completion
            QSignalBlocker dontTriggerCursorMovement(this);
            this->completer_->complete();
        }
        return;
    }

    if (!event->text().isEmpty())
    {
        this->completionInProgress_ = false;
    }

    if (!event->isAccepted())
    {
        QTextEdit::keyPressEvent(event);
    }
}

void ResizingTextEdit::focusInEvent(QFocusEvent *event)
{
    QTextEdit::focusInEvent(event);

    if (event->gotFocus())
    {
        this->focused.invoke();
    }
}

void ResizingTextEdit::focusOutEvent(QFocusEvent *event)
{
    QTextEdit::focusOutEvent(event);

    if (event->lostFocus())
    {
        this->focusLost.invoke();
    }
}

void ResizingTextEdit::setCompleter(QCompleter *c)
{
    if (this->completer_)
    {
        QObject::disconnect(this->completer_, nullptr, this, nullptr);
    }

    this->completer_ = c;

    if (!this->completer_)
    {
        return;
    }

    this->completer_->setWidget(this);
    this->completer_->setCompletionMode(QCompleter::InlineCompletion);
    this->completer_->setCaseSensitivity(Qt::CaseInsensitive);

    QObject::connect(completer_,
                     static_cast<void (QCompleter::*)(const QString &)>(
                         &QCompleter::highlighted),
                     this, &ResizingTextEdit::insertCompletion);
}

void ResizingTextEdit::resetCompletion()
{
    this->completionInProgress_ = false;
}

void ResizingTextEdit::insertCompletion(const QString &completion)
{
    if (this->completer_->widget() != this)
    {
        return;
    }

    bool hadSpace = false;
    auto prefix = this->textUnderCursor(&hadSpace);

    int prefixSize = prefix.size();

    if (hadSpace)
    {
        ++prefixSize;
    }

    QTextCursor tc = this->textCursor();
    tc.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor,
                    prefixSize);
    tc.insertText(completion);
    this->setTextCursor(tc);
}

bool ResizingTextEdit::canInsertFromMimeData(const QMimeData *source) const
{
    if (source->hasImage() || source->hasFormat("text/plain"))
    {
        return true;
    }
    return QTextEdit::canInsertFromMimeData(source);
}

void ResizingTextEdit::insertFromMimeData(const QMimeData *source)
{
    if (getSettings()->imageUploaderEnabled)
    {
        if (source->hasImage())
        {
            this->imagePasted.invoke(source);
            return;
        }

        if (source->hasUrls())
        {
            bool hasUploadable = false;
            auto mimeDb = QMimeDatabase();
            for (const QUrl &url : source->urls())
            {
                QMimeType mime = mimeDb.mimeTypeForUrl(url);
                if (mime.name().startsWith("image"))
                {
                    hasUploadable = true;
                    break;
                }
            }

            if (hasUploadable)
            {
                this->imagePasted.invoke(source);
                return;
            }
        }
    }

    insertPlainText(source->text());
}

}  // namespace chatterino
