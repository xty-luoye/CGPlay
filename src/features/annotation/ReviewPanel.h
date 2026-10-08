#pragma once
// CGPlay ReviewPanel.h — Phase 6: Comment Cards + QTabWidget
#include <QWidget>
#include <QTabWidget>
#include <QTreeWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QVector>
#include <QLabel>
#include <QVBoxLayout>
#include <QFrame>
#include <QScrollArea>
#include <QColor>
#include <QPointer>

namespace cgplay {

struct AnnotationItem;
class AnnotationManager;

class ReviewPanel : public QWidget
{
    Q_OBJECT
public:
    explicit ReviewPanel(AnnotationManager* mgr, QWidget* parent = nullptr);
    void refresh(const QVector<AnnotationItem>& all, int currentFrame = 0);
    void selectById(const QString& annId);
    void setToolsWidget(QWidget* widget);
    void setSettingsWidget(QWidget* widget);
    void setCurrentTab(int index);

Q_SIGNALS:
    void jumpToFrame(int frame);
    void annotationSelected(const QString& annId);
    void annotationDeleted(const QString& annId);
    void annotationCommentEdited(const QString& annId, const QString& text);
    void createNoteRequested(const QString& text);
    void toolSelected(const QString& toolName);

protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void _setupReviewTab(QWidget* tab);
    void _setupMetadataTab(QWidget* tab);
    void _setupInfoTab(QWidget* tab);
    void _setupSettingsTab(QWidget* tab);
    void _filter();
    void _populateCards(const QVector<AnnotationItem>& anns);
    void _onDoubleClick(QTreeWidgetItem* item, int col);
    void _onClick(QTreeWidgetItem* item, int col);
    void _deleteSelected();
    void _setSelectedCard(QFrame* card, bool emitSignal = true);
    void _clearSelection();
    void _applyCardStyle(QFrame* card, const QColor& statusColor, bool selected);
    void _requestCommentEdit(const QString& annId);

    AnnotationManager*     _mgr;
    QVector<AnnotationItem> _annotations;

    QTabWidget*   _tabWidget     = nullptr;
    QWidget*      _reviewTab     = nullptr;
    QLineEdit*    _search        = nullptr;
    QComboBox*    _statusFilter  = nullptr;
    QComboBox*    _sortCombo     = nullptr;
    QScrollArea*  _scrollArea    = nullptr;
    QWidget*      _cardContainer = nullptr;
    QVBoxLayout*  _cardLayout    = nullptr;
    QWidget*      _toolsHost     = nullptr;
    QVBoxLayout*  _toolsLayout   = nullptr;
    QWidget*      _settingsTabHost = nullptr;
    QVBoxLayout*  _settingsLayout = nullptr;
    QPointer<QWidget> _settingsWidget;
    QLabel*       _settingsPlaceholder = nullptr;
    QFrame*       _selectedCard  = nullptr;
    QString       _lastSignature;
    int           _currentFrame = 0;
};

} // namespace cgplay
