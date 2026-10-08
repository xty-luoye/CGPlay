#pragma once
#include <QWidget>
#include <QToolButton>

namespace cgplay {

class NavigationRail : public QWidget
{
    Q_OBJECT
public:
    enum Page { Playlist=0, Review=1, Compare=2, Versions=3, Timeline=4, Settings=5 };
    explicit NavigationRail(QWidget* parent=nullptr);
    Page currentPage() const { return _page; }
    QToolButton* btn(int i) const;

Q_SIGNALS:
    void pageChanged(int page);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void _select(int i);
    QToolButton* _btn[6] = {};
    Page _page = Playlist;
};

} // namespace cgplay
