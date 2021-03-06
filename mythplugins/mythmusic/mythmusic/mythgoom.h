#ifndef MYTHGOOM 
#define MYTHGOOM

#include "mainvisual.h"
#include "config.h"

using namespace std;

class Goom : public VisualBase
{
public:
    Goom(void);
    virtual ~Goom();

    void resize(const QSize &size) override; // VisualBase
    bool process(VisualNode *node) override; // VisualBase
    bool draw(QPainter *p, const QColor &back) override; // VisualBase
    void handleKeyPress(const QString &action) override // VisualBase
        {(void) action;}

private:
    QSize m_size;

    unsigned int *m_buffer;
    int m_scalew, m_scaleh;
};

#endif //MYTHGOOM
