// #703 / #1242 The new interface's widget kit: tokens, the component stylesheet and the control
// templates (llnewuikitstyle.h, no viewer dependencies), plus the viewer-side parts: fonts and
// settings bindings.
#pragma once

#include "llnewuikitstyle.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <boost/signals2/connection.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Rml
{
class DataModelConstructor;
}

// Binds gSavedSettings controls to data variables of one RmlUi data model: the variable reads and
// writes the setting, and a change of the setting (from anywhere) dirties the variable. The
// binding owns its signal connections; destroy it with the model. Bounded by its bindings.
class LLNewUISettingBinding
{
public:
    explicit LLNewUISettingBinding(Rml::DataModelHandle model) : mModel(model) {}
    // Boolean / number / string settings by control name; false when the control does not exist.
    bool bind(Rml::DataModelConstructor& model, const std::string& variable, const std::string& control);

private:
    Rml::DataModelHandle                                   mModel;
    std::vector<boost::signals2::scoped_connection>        mConnections;
};
