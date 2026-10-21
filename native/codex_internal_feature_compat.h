#ifndef CODEX_INTERNAL_FEATURE_COMPAT_H
#define CODEX_INTERNAL_FEATURE_COMPAT_H

#include <ProToolkit.h>
#include <ProFeature.h>
#include <ProElement.h>
#include <ProSelection.h>

ProError codex_internal_feature_withoptions_create(
    ProSelection model,
    ProElement elemtree,
    ProFeatureCreateOptions *options,
    int flags,
    ProFeature *feature,
    ProErrorlist *errors);

#endif
