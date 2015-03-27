/* this Plugin does nothing. It is just an example
 *
 *
 *
*/

#ifndef BEISPIEL_H
#define BEISPIEL_H

#include <cover/coVRPlugin.h>
#include <boost/smart_ptr.hpp>
#include <cover/mui/support/Listener.h>

namespace mui
{
class ToggleButton;
class TabFolder;
class Frame;
class Tab;
class ConfigManager;
class PotiSlider;
class LabelElement;
}

namespace opencover{
class coTUITab;
}


class UserInterface: public opencover::coVRPlugin, public mui::Listener
{
public:
    UserInterface();
    ~UserInterface();
    bool init();
    mui::ConfigManager *ConfigManager;
    void muiEvent(mui::Element *muiItem);

private:
    boost::shared_ptr<mui::Tab> Tab1;
    boost::shared_ptr<mui::ToggleButton> Button1;
    boost::shared_ptr<mui::ToggleButton> Button2;
    boost::shared_ptr<mui::PotiSlider> Slider1;
    boost::shared_ptr<mui::Frame> Frame;
    boost::shared_ptr<mui::LabelElement> Label;
};

#endif
