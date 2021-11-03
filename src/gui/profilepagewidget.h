#pragma once

#include <QBoxLayout>
#include <QLabel>

#include <cstddef>

namespace OCC {

class OcsProfileConnector;
struct HovercardAction;

class ProfilePageWidget : public QWidget
{
    Q_OBJECT
public:
    explicit ProfilePageWidget(QWidget *parent = nullptr);
    ~ProfilePageWidget() override;

    void setProfileConnector(std::unique_ptr<OcsProfileConnector> profileConnector, const QString &userId);

private:
    void onHovercardFetched();
    void onIconLoaded(const std::size_t &hovercardActionIndex);

    void recreateLayout();
    void resetLayout();
    void createLayout();
    void displayHovercardActions(const std::vector<HovercardAction> &hovercardActions);
    void displayNoHovercardActions();

    std::unique_ptr<OcsProfileConnector> _profileConnector;

    QVBoxLayout *_mainLayout{};
    std::vector<QLabel *> _profilePageButtonIcons;
};
}
