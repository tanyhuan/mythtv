#ifndef BOOKMARKMANAGER_H
#define BOOKMARKMANAGER_H

// libmythbase
#include <mythmiscutil.h>

// libmythui
#include <mythscreentype.h>

class MythBrowser;
class MythDialogBox;

class Bookmark
{
  public:
    Bookmark(void)
        : category(""), name(""), url(""), isHomepage(false), selected(false)
    {
        ;
    }

    QString category;
    QString name;
    QString sortName;
    QString url;
    bool    isHomepage;
    bool    selected;

    inline bool operator == (const Bookmark &b) const
    {
        return category == b.category && name == b.name && url == b.url;
    }
    static inline bool sortByName(Bookmark *a, Bookmark *b)
        { return naturalCompare(a->sortName, b->sortName) < 0; }
};

class BrowserConfig : public MythScreenType
{
  Q_OBJECT

  public:

    BrowserConfig(MythScreenStack *parent, const char *name = nullptr);
    ~BrowserConfig() = default;

    bool Create(void) override; // MythScreenType
    bool keyPressEvent(QKeyEvent *) override; // MythScreenType

  private:
    MythUITextEdit   *m_commandEdit;
    MythUITextEdit   *m_zoomEdit;

    MythUIText       *m_descriptionText;
    MythUIText       *m_titleText;
    MythUICheckBox   *m_enablePluginsCheck;

    MythUIButton     *m_okButton;
    MythUIButton     *m_cancelButton;

  private slots:
    void slotSave(void);
    void slotFocusChanged(void);
};

class BookmarkManager : public MythScreenType
{
  Q_OBJECT

  public:
    BookmarkManager(MythScreenStack *parent, const char *name);
    ~BookmarkManager();

    bool Create(void) override; // MythScreenType
    bool keyPressEvent(QKeyEvent *) override; // MythScreenType

  private slots:
    void slotGroupSelected(MythUIButtonListItem *item);
    void slotBookmarkClicked(MythUIButtonListItem *item);
    void slotEditDialogExited(void);
    void slotDoDeleteCurrent(bool doDelete);
    void slotDoDeleteMarked(bool doDelete);
    void slotBrowserClosed(void);

    void slotSettings(void);
    void slotSetHomepage(void);
    void slotAddBookmark(void);
    void slotEditBookmark(void);
    void slotDeleteCurrent(void);
    void slotDeleteMarked(void);
    void slotShowCurrent(void);
    void slotShowMarked(void);
    void slotClearMarked(void);

  private:
    uint GetMarkedCount(void);
    void UpdateGroupList(void);
    void UpdateURLList(void);
    void ShowEditDialog(bool edit);
    void ReloadBookmarks(void);

    QList<Bookmark*>  m_siteList;

    Bookmark          m_savedBookmark;

    MythUIButtonList *m_bookmarkList;
    MythUIButtonList   *m_groupList;
    MythUIText       *m_messageText;

    MythDialogBox    *m_menuPopup;
};

Q_DECLARE_METATYPE(Bookmark *)

#endif
