#include "pch.h"

#include "F5Lut_Common.h"

#include <dlssnr/DlssNrFeature_Dx12.h>
#include <dlssnr/DlssNrFeature_Vk.h>

bool F5Lut_Common::NrGradesTheStack()
{
    if (!Config::Instance()->DlssNrEnabled.value_or_default())
        return false;

    return DlssNr::IsRunning() || DlssNr::IsRunningVk() || DlssNr::PostLutGradedAfterNrWithin(250);
}
