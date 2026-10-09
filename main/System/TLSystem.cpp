#include "TLSystem.hpp"
#include "TLConfig.hpp"

#include <algorithm>
#include <cctype>

#include "../Utils/inicpp.hpp"

#include "../Plugins/ChiselDB.hpp"
#include "../Plugins/CHIronCLogB.hpp"


void TLInitialize(TLSequencer** tltest, PluginManager** plugins, std::function<void(TLLocalConfig&)> tlcfgInit)
{
    // internal event handlers
    Gravity::RegisterListener(
        Gravity::MakeListener(
            "tltest.logger.plugins.enable", 0,
            (std::function<void(PluginEvent::PostEnable&)>)
            [] (PluginEvent::PostEnable& event) -> void {
                LogInfo("PIM", Append("---PluginManager------------------------------------------------").EndLine());
                LogInfo("PIM", Append("Enabled Plugin:").EndLine());
                LogInfo("PIM", Append(" - name          : ", event.GetPlugin()->GetName()).EndLine());
                LogInfo("PIM", Append(" - display_name  : ", event.GetPlugin()->GetDisplayName()).EndLine());
                LogInfo("PIM", Append(" - version       : ", event.GetPlugin()->GetVersion()).EndLine());
                LogInfo("PIM", Append(" - description   : ", event.GetPlugin()->GetDescription()).EndLine());
                LogInfo("PIM", Append("----------------------------------------------------------------").EndLine());
            }
        )
    );

    Gravity::RegisterListener(
        Gravity::MakeListener(
            "tltest.logger.plugins.disable", 0,
            (std::function<void(PluginEvent::PostDisable&)>)
            [] (PluginEvent::PostDisable& event) -> void {
                LogInfo("PIM", Append("---PluginManager------------------------------------------------").EndLine());
                LogInfo("PIM", Append("Disabled Plugin:").EndLine());
                LogInfo("PIM", Append(" - name          : ", event.GetPlugin()->GetName()).EndLine());
                LogInfo("PIM", Append(" - display_name  : ", event.GetPlugin()->GetDisplayName()).EndLine());
                LogInfo("PIM", Append(" - version       : ", event.GetPlugin()->GetVersion()).EndLine());
                LogInfo("PIM", Append(" - description   : ", event.GetPlugin()->GetDescription()).EndLine());
                LogInfo("PIM", Append("----------------------------------------------------------------").EndLine());
            }
        )
    ); 

    TLLocalConfig tlcfg = LoadTestConfig(tlcfgInit);

    //
    (*tltest) = new TLSequencer;
    (*tltest)->Initialize(tlcfg);

    (*plugins) = new PluginManager;
    (*plugins)->EnablePlugin(new ChiselDB::PluginInstance);
    (*plugins)->EnablePlugin(new CHIron::CLogB::PluginInstance);
}

void TLFinalize(TLSequencer** tltest, PluginManager** plugins)
{
    (*plugins)->DisableAll([](auto plugin) -> void { delete plugin; });
    delete *plugins;
    
    (*tltest)->Finalize();
    delete *tltest;
}
