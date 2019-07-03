/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "kitinformation.h"

#include "abi.h"
#include "devicesupport/desktopdevice.h"
#include "devicesupport/devicemanager.h"
#include "devicesupport/devicemanagermodel.h"
#include "devicesupport/idevicefactory.h"
#include "projectexplorerconstants.h"
#include "kit.h"
#include "toolchain.h"
#include "toolchainmanager.h"

#include <coreplugin/icore.h>
#include <coreplugin/variablechooser.h>
#include <ssh/sshconnection.h>
#include <utils/algorithm.h>
#include <utils/environment.h>
#include <utils/environmentdialog.h>
#include <utils/macroexpander.h>
#include <utils/pathchooser.h>
#include <utils/qtcassert.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace ProjectExplorer {

const char KITINFORMATION_ID_V1[] = "PE.Profile.ToolChain";
const char KITINFORMATION_ID_V2[] = "PE.Profile.ToolChains";
const char KITINFORMATION_ID_V3[] = "PE.Profile.ToolChainsV3";

// --------------------------------------------------------------------------
// SysRootKitAspect:
// --------------------------------------------------------------------------

namespace Internal {
class SysRootKitAspectWidget : public KitAspectWidget
{
    Q_DECLARE_TR_FUNCTIONS(ProjectExplorer::SysRootKitAspect)

public:
    SysRootKitAspectWidget(Kit *k, const KitAspect *ki) : KitAspectWidget(k, ki)
    {
        m_chooser = new Utils::PathChooser;
        m_chooser->setExpectedKind(Utils::PathChooser::ExistingDirectory);
        m_chooser->setHistoryCompleter(QLatin1String("PE.SysRoot.History"));
        m_chooser->setFileName(SysRootKitAspect::sysRoot(k));
        connect(m_chooser, &Utils::PathChooser::pathChanged,
                this, &SysRootKitAspectWidget::pathWasChanged);
    }

    ~SysRootKitAspectWidget() override { delete m_chooser; }

private:
    void makeReadOnly() override { m_chooser->setReadOnly(true); }
    QWidget *buttonWidget() const override { return m_chooser->buttonAtIndex(0); }
    QWidget *mainWidget() const override { return m_chooser->lineEdit(); }

    void refresh() override
    {
        if (!m_ignoreChange)
            m_chooser->setFileName(SysRootKitAspect::sysRoot(m_kit));
    }

    void setPalette(const QPalette &p) override
    {
        KitAspectWidget::setPalette(p);
        m_chooser->setOkColor(p.color(QPalette::Active, QPalette::Text));
    }

    void pathWasChanged()
    {
        m_ignoreChange = true;
        SysRootKitAspect::setSysRoot(m_kit, m_chooser->fileName());
        m_ignoreChange = false;
    }

    Utils::PathChooser *m_chooser;
    bool m_ignoreChange = false;
};
} // namespace Internal

SysRootKitAspect::SysRootKitAspect()
{
    setObjectName(QLatin1String("SysRootInformation"));
    setId(SysRootKitAspect::id());
    setDisplayName(tr("Sysroot"));
    setDescription(tr("The root directory of the system image to use.<br>"
                      "Leave empty when building for the desktop."));
    setPriority(31000);
}

Tasks SysRootKitAspect::validate(const Kit *k) const
{
    Tasks result;
    const Utils::FilePath dir = SysRootKitAspect::sysRoot(k);
    if (dir.isEmpty())
        return result;

    if (dir.toString().startsWith("target:") || dir.toString().startsWith("remote:"))
        return result;

    const QFileInfo fi = dir.toFileInfo();

    if (!fi.exists()) {
        result << Task(Task::Warning, tr("Sys Root \"%1\" does not exist in the file system.").arg(dir.toUserOutput()),
                       Utils::FilePath(), -1, Core::Id(Constants::TASK_CATEGORY_BUILDSYSTEM));
    } else if (!fi.isDir()) {
        result << Task(Task::Warning, tr("Sys Root \"%1\" is not a directory.").arg(dir.toUserOutput()),
                       Utils::FilePath(), -1, Core::Id(Constants::TASK_CATEGORY_BUILDSYSTEM));
    } else if (QDir(dir.toString()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()) {
        result << Task(Task::Warning, tr("Sys Root \"%1\" is empty.").arg(dir.toUserOutput()),
                       Utils::FilePath(), -1, Core::Id(Constants::TASK_CATEGORY_BUILDSYSTEM));
    }
    return result;
}

KitAspectWidget *SysRootKitAspect::createConfigWidget(Kit *k) const
{
    QTC_ASSERT(k, return nullptr);

    return new Internal::SysRootKitAspectWidget(k, this);
}

KitAspect::ItemList SysRootKitAspect::toUserOutput(const Kit *k) const
{
    return {{tr("Sys Root"), sysRoot(k).toUserOutput()}};
}

void SysRootKitAspect::addToMacroExpander(Kit *kit, Utils::MacroExpander *expander) const
{
    QTC_ASSERT(kit, return);

    expander->registerFileVariables("SysRoot", tr("Sys Root"), [kit]() -> QString {
        return SysRootKitAspect::sysRoot(kit).toString();
    });
}

Core::Id SysRootKitAspect::id()
{
    return "PE.Profile.SysRoot";
}

Utils::FilePath SysRootKitAspect::sysRoot(const Kit *k)
{
    if (!k)
        return Utils::FilePath();

    if (!k->value(SysRootKitAspect::id()).toString().isEmpty())
        return Utils::FilePath::fromString(k->value(SysRootKitAspect::id()).toString());

    for (ToolChain *tc : ToolChainKitAspect::toolChains(k)) {
        if (!tc->sysRoot().isEmpty())
            return Utils::FilePath::fromString(tc->sysRoot());
    }

    return Utils::FilePath();
}

void SysRootKitAspect::setSysRoot(Kit *k, const Utils::FilePath &v)
{
    if (!k)
        return;

    for (ToolChain *tc : ToolChainKitAspect::toolChains(k)) {
        if (!tc->sysRoot().isEmpty()) {
            // It's the sysroot from toolchain, don't set it.
            if (tc->sysRoot() == v.toString())
                return;

            // We've changed the default toolchain sysroot, set it.
            break;
        }
    }
    k->setValue(SysRootKitAspect::id(), v.toString());
}

// --------------------------------------------------------------------------
// ToolChainKitAspect:
// --------------------------------------------------------------------------

namespace Internal {
class ToolChainKitAspectWidget : public KitAspectWidget
{
    Q_DECLARE_TR_FUNCTIONS(ProjectExplorer::ToolChainKitAspect)

public:
    ToolChainKitAspectWidget(Kit *k, const KitAspect *ki) : KitAspectWidget(k, ki)
    {
        m_mainWidget = new QWidget;
        m_mainWidget->setContentsMargins(0, 0, 0, 0);

        auto layout = new QGridLayout(m_mainWidget);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setColumnStretch(1, 2);

        QList<Core::Id> languageList = Utils::toList(ToolChainManager::allLanguages());
        Utils::sort(languageList, [](Core::Id l1, Core::Id l2) {
            return ToolChainManager::displayNameOfLanguageId(l1)
                    < ToolChainManager::displayNameOfLanguageId(l2);
        });
        QTC_ASSERT(!languageList.isEmpty(), return);
        int row = 0;
        for (Core::Id l : qAsConst(languageList)) {
            layout->addWidget(new QLabel(ToolChainManager::displayNameOfLanguageId(l) + ':'), row, 0);
            auto cb = new QComboBox;
            cb->setSizePolicy(QSizePolicy::Ignored, cb->sizePolicy().verticalPolicy());
            cb->setToolTip(ki->description());

            m_languageComboboxMap.insert(l, cb);
            layout->addWidget(cb, row, 1);
            ++row;

            connect(cb, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, [this, l](int idx) { currentToolChainChanged(l, idx); });
        }

        refresh();

        m_manageButton = new QPushButton(KitAspectWidget::msgManage());
        m_manageButton->setContentsMargins(0, 0, 0, 0);
        connect(m_manageButton, &QAbstractButton::clicked,
                this, &ToolChainKitAspectWidget::manageToolChains);
    }

    ~ToolChainKitAspectWidget() override
    {
        delete m_mainWidget;
        delete m_manageButton;
    }

private:
    QWidget *mainWidget() const override { return m_mainWidget; }
    QWidget *buttonWidget() const override { return m_manageButton; }

    void refresh() override
    {
        m_ignoreChanges = true;
        foreach (Core::Id l, m_languageComboboxMap.keys()) {
            const QList<ToolChain *> ltcList
                    = ToolChainManager::toolChains(Utils::equal(&ToolChain::language, l));

            QComboBox *cb = m_languageComboboxMap.value(l);
            cb->clear();
            cb->addItem(tr("<No compiler>"), QByteArray());

            foreach (ToolChain *tc, ltcList)
                cb->addItem(tc->displayName(), tc->id());

            cb->setEnabled(cb->count() > 1 && !m_isReadOnly);
            const int index = indexOf(cb, ToolChainKitAspect::toolChain(m_kit, l));
            cb->setCurrentIndex(index);
        }
        m_ignoreChanges = false;
    }

    void makeReadOnly() override
    {
        m_isReadOnly = true;
        foreach (Core::Id l, m_languageComboboxMap.keys()) {
            m_languageComboboxMap.value(l)->setEnabled(false);
        }
    }

    void manageToolChains()
    {
        Core::ICore::showOptionsDialog(Constants::TOOLCHAIN_SETTINGS_PAGE_ID, buttonWidget());
    }

    void currentToolChainChanged(Core::Id language, int idx)
    {
        if (m_ignoreChanges || idx < 0)
            return;

        const QByteArray id = m_languageComboboxMap.value(language)->itemData(idx).toByteArray();
        ToolChain *tc = ToolChainManager::findToolChain(id);
        QTC_ASSERT(!tc || tc->language() == language, return);
        if (tc)
            ToolChainKitAspect::setToolChain(m_kit, tc);
        else
            ToolChainKitAspect::clearToolChain(m_kit, language);
    }

    int indexOf(QComboBox *cb, const ToolChain *tc)
    {
        const QByteArray id = tc ? tc->id() : QByteArray();
        for (int i = 0; i < cb->count(); ++i) {
            if (id == cb->itemData(i).toByteArray())
                return i;
        }
        return -1;
    }

    QWidget *m_mainWidget = nullptr;
    QPushButton *m_manageButton = nullptr;
    QHash<Core::Id, QComboBox *> m_languageComboboxMap;
    bool m_ignoreChanges = false;
    bool m_isReadOnly = false;
};
} // namespace Internal

ToolChainKitAspect::ToolChainKitAspect()
{
    setObjectName(QLatin1String("ToolChainInformation"));
    setId(ToolChainKitAspect::id());
    setDisplayName(tr("Compiler"));
    setDescription(tr("The compiler to use for building.<br>"
                      "Make sure the compiler will produce binaries compatible "
                      "with the target device, Qt version and other libraries used."));
    setPriority(30000);

    connect(KitManager::instance(), &KitManager::kitsLoaded,
            this, &ToolChainKitAspect::kitsWereLoaded);
}

// language id -> tool chain id
static QMap<Core::Id, QByteArray> defaultToolChainIds()
{
    QMap<Core::Id, QByteArray> toolChains;
    const Abi abi = Abi::hostAbi();
    QList<ToolChain *> tcList = ToolChainManager::toolChains(Utils::equal(&ToolChain::targetAbi, abi));
    foreach (Core::Id l, ToolChainManager::allLanguages()) {
        ToolChain *tc = Utils::findOrDefault(tcList, Utils::equal(&ToolChain::language, l));
        toolChains.insert(l, tc ? tc->id() : QByteArray());
    }
    return toolChains;
}

static QVariant defaultToolChainValue()
{
    const QMap<Core::Id, QByteArray> toolChains = defaultToolChainIds();
    QVariantMap result;
    auto end = toolChains.end();
    for (auto it = toolChains.begin(); it != end; ++it) {
        result.insert(it.key().toString(), it.value());
    }
    return result;
}

Tasks ToolChainKitAspect::validate(const Kit *k) const
{
    Tasks result;

    const QList<ToolChain*> tcList = toolChains(k);
    if (tcList.isEmpty()) {
        result << Task(Task::Warning, ToolChainKitAspect::msgNoToolChainInTarget(),
                       Utils::FilePath(), -1, Core::Id(Constants::TASK_CATEGORY_BUILDSYSTEM));
    } else {
        QSet<Abi> targetAbis;
        foreach (ToolChain *tc, tcList) {
            targetAbis.insert(tc->targetAbi());
            result << tc->validateKit(k);
        }
        if (targetAbis.count() != 1) {
            result << Task(Task::Error, tr("Compilers produce code for different ABIs: %1")
                           .arg(Utils::transform<QList>(targetAbis, &Abi::toString).join(", ")),
                           Utils::FilePath(), -1, Core::Id(Constants::TASK_CATEGORY_BUILDSYSTEM));
        }
    }
    return result;
}

void ToolChainKitAspect::upgrade(Kit *k)
{
    QTC_ASSERT(k, return);

    const Core::Id oldIdV1 = KITINFORMATION_ID_V1;
    const Core::Id oldIdV2 = KITINFORMATION_ID_V2;

    // upgrade <=4.1 to 4.2 (keep old settings around for now)
    {
        const QVariant oldValue = k->value(oldIdV1);
        const QVariant value = k->value(oldIdV2);
        if (value.isNull() && !oldValue.isNull()) {
            QVariantMap newValue;
            if (oldValue.type() == QVariant::Map) {
                // Used between 4.1 and 4.2:
                newValue = oldValue.toMap();
            } else {
                // Used up to 4.1:
                newValue.insert(Deprecated::Toolchain::languageId(Deprecated::Toolchain::Cxx), oldValue.toString());

                const Core::Id typeId = DeviceTypeKitAspect::deviceTypeId(k);
                if (typeId == Constants::DESKTOP_DEVICE_TYPE) {
                    // insert default C compiler which did not exist before
                    newValue.insert(Deprecated::Toolchain::languageId(Deprecated::Toolchain::C),
                                    defaultToolChainIds().value(Core::Id(Constants::C_LANGUAGE_ID)));
                }
            }
            k->setValue(oldIdV2, newValue);
            k->setSticky(oldIdV2, k->isSticky(oldIdV1));
        }
    }

    // upgrade 4.2 to 4.3 (keep old settings around for now)
    {
        const QVariant oldValue = k->value(oldIdV2);
        const QVariant value = k->value(ToolChainKitAspect::id());
        if (value.isNull() && !oldValue.isNull()) {
            QVariantMap newValue = oldValue.toMap();
            QVariantMap::iterator it = newValue.find(Deprecated::Toolchain::languageId(Deprecated::Toolchain::C));
            if (it != newValue.end())
                newValue.insert(Core::Id(Constants::C_LANGUAGE_ID).toString(), it.value());
            it = newValue.find(Deprecated::Toolchain::languageId(Deprecated::Toolchain::Cxx));
            if (it != newValue.end())
                newValue.insert(Core::Id(Constants::CXX_LANGUAGE_ID).toString(), it.value());
            k->setValue(ToolChainKitAspect::id(), newValue);
            k->setSticky(ToolChainKitAspect::id(), k->isSticky(oldIdV2));
        }
    }

    // upgrade 4.3-temporary-master-state to 4.3:
    {
        const QVariantMap valueMap = k->value(ToolChainKitAspect::id()).toMap();
        QVariantMap result;
        for (const QString &key : valueMap.keys()) {
            const int pos = key.lastIndexOf('.');
            if (pos >= 0)
                result.insert(key.mid(pos + 1), valueMap.value(key));
            else
                result.insert(key, valueMap.value(key));
        }
        k->setValue(ToolChainKitAspect::id(), result);
    }
}

void ToolChainKitAspect::fix(Kit *k)
{
    QTC_ASSERT(ToolChainManager::isLoaded(), return);
    foreach (const Core::Id& l, ToolChainManager::allLanguages()) {
        const QByteArray tcId = toolChainId(k, l);
        if (!tcId.isEmpty() && !ToolChainManager::findToolChain(tcId)) {
            qWarning("Tool chain set up in kit \"%s\" for \"%s\" not found.",
                     qPrintable(k->displayName()),
                     qPrintable(ToolChainManager::displayNameOfLanguageId(l)));
            clearToolChain(k, l); // make sure to clear out no longer known tool chains
        }
    }
}

static Core::Id findLanguage(const QString &ls)
{
    QString lsUpper = ls.toUpper();
    return Utils::findOrDefault(ToolChainManager::allLanguages(),
                         [lsUpper](Core::Id l) { return lsUpper == l.toString().toUpper(); });
}

void ToolChainKitAspect::setup(Kit *k)
{
    QTC_ASSERT(ToolChainManager::isLoaded(), return);
    QTC_ASSERT(k, return);

    QVariantMap value = k->value(id()).toMap();
    if (value.empty())
        value = defaultToolChainValue().toMap();

    for (auto i = value.constBegin(); i != value.constEnd(); ++i) {
        Core::Id l = findLanguage(i.key());

        if (!l.isValid())
            continue;

        const QByteArray id = i.value().toByteArray();
        ToolChain *tc = ToolChainManager::findToolChain(id);
        if (tc)
            continue;

        // ID is not found: Might be an ABI string...
        const QString abi = QString::fromUtf8(id);
        tc = ToolChainManager::toolChain([abi, l](const ToolChain *t) {
                 return t->targetAbi().toString() == abi && t->language() == l;
             });
        if (tc)
            setToolChain(k, tc);
        else
            clearToolChain(k, l);
    }
}

KitAspectWidget *ToolChainKitAspect::createConfigWidget(Kit *k) const
{
    QTC_ASSERT(k, return nullptr);
    return new Internal::ToolChainKitAspectWidget(k, this);
}

QString ToolChainKitAspect::displayNamePostfix(const Kit *k) const
{
    ToolChain *tc = toolChain(k, Constants::CXX_LANGUAGE_ID);
    return tc ? tc->displayName() : QString();
}

KitAspect::ItemList ToolChainKitAspect::toUserOutput(const Kit *k) const
{
    ToolChain *tc = toolChain(k, Constants::CXX_LANGUAGE_ID);
    return {{tr("Compiler"), tc ? tc->displayName() : tr("None")}};
}

void ToolChainKitAspect::addToEnvironment(const Kit *k, Utils::Environment &env) const
{
    ToolChain *tc = toolChain(k, Constants::CXX_LANGUAGE_ID);
    if (tc)
        tc->addToEnvironment(env);
}

void ToolChainKitAspect::addToMacroExpander(Kit *kit, Utils::MacroExpander *expander) const
{
    QTC_ASSERT(kit, return);

    // Compatibility with Qt Creator < 4.2:
    expander->registerVariable("Compiler:Name", tr("Compiler"),
                               [kit]() -> QString {
                                   const ToolChain *tc = toolChain(kit, Constants::CXX_LANGUAGE_ID);
                                   return tc ? tc->displayName() : tr("None");
                               });

    expander->registerVariable("Compiler:Executable", tr("Path to the compiler executable"),
                               [kit]() -> QString {
                                   const ToolChain *tc = toolChain(kit, Constants::CXX_LANGUAGE_ID);
                                   return tc ? tc->compilerCommand().toString() : QString();
                               });

    expander->registerPrefix("Compiler:Name", tr("Compiler for different languages"),
                             [kit](const QString &ls) -> QString {
                                 const ToolChain *tc = toolChain(kit, findLanguage(ls));
                                 return tc ? tc->displayName() : tr("None");
                             });
    expander->registerPrefix("Compiler:Executable", tr("Compiler executable for different languages"),
                             [kit](const QString &ls) -> QString {
                                 const ToolChain *tc = toolChain(kit, findLanguage(ls));
                                 return tc ? tc->compilerCommand().toString() : QString();
                             });
}


IOutputParser *ToolChainKitAspect::createOutputParser(const Kit *k) const
{
    for (const Core::Id langId : {Constants::CXX_LANGUAGE_ID, Constants::C_LANGUAGE_ID}) {
        if (const ToolChain * const tc = toolChain(k, langId))
            return tc->outputParser();
    }
    return nullptr;
}

QSet<Core::Id> ToolChainKitAspect::availableFeatures(const Kit *k) const
{
    QSet<Core::Id> result;
    for (ToolChain *tc : toolChains(k))
        result.insert(tc->typeId().withPrefix("ToolChain."));
    return result;
}

Core::Id ToolChainKitAspect::id()
{
    return KITINFORMATION_ID_V3;
}

QByteArray ToolChainKitAspect::toolChainId(const Kit *k, Core::Id language)
{
    QTC_ASSERT(ToolChainManager::isLoaded(), return nullptr);
    if (!k)
        return QByteArray();
    QVariantMap value = k->value(ToolChainKitAspect::id()).toMap();
    return value.value(language.toString(), QByteArray()).toByteArray();
}

ToolChain *ToolChainKitAspect::toolChain(const Kit *k, Core::Id language)
{
    return ToolChainManager::findToolChain(toolChainId(k, language));
}

QList<ToolChain *> ToolChainKitAspect::toolChains(const Kit *k)
{
    QTC_ASSERT(k, return QList<ToolChain *>());

    const QVariantMap value = k->value(ToolChainKitAspect::id()).toMap();
    const QList<ToolChain *> tcList
            = Utils::transform<QList>(ToolChainManager::allLanguages(),
                               [&value](Core::Id l) -> ToolChain * {
                                   return ToolChainManager::findToolChain(value.value(l.toString()).toByteArray());
                               });
    return Utils::filtered(tcList, [](ToolChain *tc) { return tc; });
}

void ToolChainKitAspect::setToolChain(Kit *k, ToolChain *tc)
{
    QTC_ASSERT(tc, return);
    QTC_ASSERT(k, return);
    QVariantMap result = k->value(ToolChainKitAspect::id()).toMap();
    result.insert(tc->language().toString(), tc->id());

    k->setValue(id(), result);
}

/**
 * @brief ToolChainKitAspect::setAllToolChainsToMatch
 *
 * Set up all toolchains to be similar to the one toolchain provided. Similar ideally means
 * that all toolchains use the "same" compiler from the same installation, but we will
 * settle for a toolchain with a matching API instead.
 *
 * @param k The kit to set up
 * @param tc The toolchain to match other languages for.
 */
void ToolChainKitAspect::setAllToolChainsToMatch(Kit *k, ToolChain *tc)
{
    QTC_ASSERT(tc, return);
    QTC_ASSERT(k, return);

    const QList<ToolChain *> allTcList = ToolChainManager::toolChains();
    QTC_ASSERT(allTcList.contains(tc), return);

    QVariantMap result = k->value(ToolChainKitAspect::id()).toMap();
    result.insert(tc->language().toString(), tc->id());

    for (Core::Id l : ToolChainManager::allLanguages()) {
        if (l == tc->language())
            continue;

        ToolChain *match = nullptr;
        ToolChain *bestMatch = nullptr;
        for (ToolChain *other : allTcList) {
            if (!other->isValid() || other->language() != l)
                continue;
            if (other->targetAbi() == tc->targetAbi())
                match = other;
            if (match == other
                    && other->compilerCommand().parentDir() == tc->compilerCommand().parentDir()) {
                bestMatch = other;
                break;
            }
        }
        if (bestMatch)
            result.insert(l.toString(), bestMatch->id());
        else if (match)
            result.insert(l.toString(), match->id());
        else
            result.insert(l.toString(), QByteArray());
    }

    k->setValue(id(), result);
}

void ToolChainKitAspect::clearToolChain(Kit *k, Core::Id language)
{
    QTC_ASSERT(language.isValid(), return);
    QTC_ASSERT(k, return);

    QVariantMap result = k->value(ToolChainKitAspect::id()).toMap();
    result.insert(language.toString(), QByteArray());
    k->setValue(id(), result);
}

Abi ToolChainKitAspect::targetAbi(const Kit *k)
{
    QList<ToolChain *> tcList = toolChains(k);
    // Find the best possible ABI for all the tool chains...
    Abi cxxAbi;
    QHash<Abi, int> abiCount;
    foreach (ToolChain *tc, tcList) {
        Abi ta = tc->targetAbi();
        if (tc->language() == Core::Id(Constants::CXX_LANGUAGE_ID))
            cxxAbi = tc->targetAbi();
        abiCount[ta] = (abiCount.contains(ta) ? abiCount[ta] + 1 : 1);
    }
    QVector<Abi> candidates;
    int count = -1;
    candidates.reserve(tcList.count());
    for (auto i = abiCount.begin(); i != abiCount.end(); ++i) {
        if (i.value() > count) {
            candidates.clear();
            candidates.append(i.key());
            count = i.value();
        } else if (i.value() == count) {
            candidates.append(i.key());
        }
    }

    // Found a good candidate:
    if (candidates.isEmpty())
        return Abi::hostAbi();
    if (candidates.contains(cxxAbi)) // Use Cxx compiler as a tie breaker
        return cxxAbi;
    return candidates.at(0); // Use basically a random Abi...
}

QString ToolChainKitAspect::msgNoToolChainInTarget()
{
    return tr("No compiler set in kit.");
}

void ToolChainKitAspect::kitsWereLoaded()
{
    foreach (Kit *k, KitManager::kits())
        fix(k);

    connect(ToolChainManager::instance(), &ToolChainManager::toolChainRemoved,
            this, &ToolChainKitAspect::toolChainRemoved);
    connect(ToolChainManager::instance(), &ToolChainManager::toolChainUpdated,
            this, &ToolChainKitAspect::toolChainUpdated);
}

void ToolChainKitAspect::toolChainUpdated(ToolChain *tc)
{
    for (Kit *k : KitManager::kits([tc](const Kit *k) { return toolChain(k, tc->language()) == tc; }))
        notifyAboutUpdate(k);
}

void ToolChainKitAspect::toolChainRemoved(ToolChain *tc)
{
    Q_UNUSED(tc);
    foreach (Kit *k, KitManager::kits())
        fix(k);
}

// --------------------------------------------------------------------------
// DeviceTypeKitAspect:
// --------------------------------------------------------------------------
namespace Internal {
class DeviceTypeKitAspectWidget : public KitAspectWidget
{
    Q_DECLARE_TR_FUNCTIONS(ProjectExplorer::DeviceTypeKitAspect)

public:
    DeviceTypeKitAspectWidget(Kit *workingCopy, const KitAspect *ki)
        : KitAspectWidget(workingCopy, ki), m_comboBox(new QComboBox)
    {
        for (IDeviceFactory *factory : IDeviceFactory::allDeviceFactories())
            m_comboBox->addItem(factory->displayName(), factory->deviceType().toSetting());
        m_comboBox->setToolTip(ki->description());
        refresh();
        connect(m_comboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &DeviceTypeKitAspectWidget::currentTypeChanged);
    }

    ~DeviceTypeKitAspectWidget() override { delete m_comboBox; }

private:
    QWidget *mainWidget() const override { return m_comboBox; }
    void makeReadOnly() override { m_comboBox->setEnabled(false); }

    void refresh() override
    {
        Core::Id devType = DeviceTypeKitAspect::deviceTypeId(m_kit);
        if (!devType.isValid())
            m_comboBox->setCurrentIndex(-1);
        for (int i = 0; i < m_comboBox->count(); ++i) {
            if (m_comboBox->itemData(i) == devType.toSetting()) {
                m_comboBox->setCurrentIndex(i);
                break;
            }
        }
    }

    void currentTypeChanged(int idx)
    {
        Core::Id type = idx < 0 ? Core::Id() : Core::Id::fromSetting(m_comboBox->itemData(idx));
        DeviceTypeKitAspect::setDeviceTypeId(m_kit, type);
    }

    QComboBox *m_comboBox;
};
} // namespace Internal

DeviceTypeKitAspect::DeviceTypeKitAspect()
{
    setObjectName(QLatin1String("DeviceTypeInformation"));
    setId(DeviceTypeKitAspect::id());
    setDisplayName(tr("Device type"));
    setDescription(tr("The type of device to run applications on."));
    setPriority(33000);
    makeEssential();
}

void DeviceTypeKitAspect::setup(Kit *k)
{
    if (k && !k->hasValue(id()))
        k->setValue(id(), QByteArray(Constants::DESKTOP_DEVICE_TYPE));
}

Tasks DeviceTypeKitAspect::validate(const Kit *k) const
{
    Q_UNUSED(k);
    return {};
}

KitAspectWidget *DeviceTypeKitAspect::createConfigWidget(Kit *k) const
{
    QTC_ASSERT(k, return nullptr);
    return new Internal::DeviceTypeKitAspectWidget(k, this);
}

KitAspect::ItemList DeviceTypeKitAspect::toUserOutput(const Kit *k) const
{
    QTC_ASSERT(k, return {});
    Core::Id type = deviceTypeId(k);
    QString typeDisplayName = tr("Unknown device type");
    if (type.isValid()) {
        if (IDeviceFactory *factory = IDeviceFactory::find(type))
            typeDisplayName = factory->displayName();
    }
    return {{tr("Device type"), typeDisplayName}};
}

const Core::Id DeviceTypeKitAspect::id()
{
    return "PE.Profile.DeviceType";
}

const Core::Id DeviceTypeKitAspect::deviceTypeId(const Kit *k)
{
    return k ? Core::Id::fromSetting(k->value(DeviceTypeKitAspect::id())) : Core::Id();
}

void DeviceTypeKitAspect::setDeviceTypeId(Kit *k, Core::Id type)
{
    QTC_ASSERT(k, return);
    k->setValue(DeviceTypeKitAspect::id(), type.toSetting());
}

QSet<Core::Id> DeviceTypeKitAspect::supportedPlatforms(const Kit *k) const
{
    return {deviceTypeId(k)};
}

QSet<Core::Id> DeviceTypeKitAspect::availableFeatures(const Kit *k) const
{
    Core::Id id = DeviceTypeKitAspect::deviceTypeId(k);
    if (id.isValid())
        return {id.withPrefix("DeviceType.")};
    return QSet<Core::Id>();
}

// --------------------------------------------------------------------------
// DeviceKitAspect:
// --------------------------------------------------------------------------
namespace Internal {
class DeviceKitAspectWidget : public KitAspectWidget
{
    Q_DECLARE_TR_FUNCTIONS(ProjectExplorer::DeviceKitAspect)

public:
    DeviceKitAspectWidget(Kit *workingCopy, const KitAspect *ki)
        : KitAspectWidget(workingCopy, ki), m_comboBox(new QComboBox),
        m_model(new DeviceManagerModel(DeviceManager::instance()))
    {
        m_comboBox->setSizePolicy(QSizePolicy::Ignored, m_comboBox->sizePolicy().verticalPolicy());
        m_comboBox->setModel(m_model);
        m_manageButton = new QPushButton(KitAspectWidget::msgManage());
        refresh();
        m_comboBox->setToolTip(ki->description());

        connect(m_model, &QAbstractItemModel::modelAboutToBeReset,
                this, &DeviceKitAspectWidget::modelAboutToReset);
        connect(m_model, &QAbstractItemModel::modelReset,
                this, &DeviceKitAspectWidget::modelReset);
        connect(m_comboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &DeviceKitAspectWidget::currentDeviceChanged);
        connect(m_manageButton, &QAbstractButton::clicked,
                this, &DeviceKitAspectWidget::manageDevices);
    }

    ~DeviceKitAspectWidget() override
    {
        delete m_comboBox;
        delete m_model;
        delete m_manageButton;
    }

private:
    QWidget *mainWidget() const override { return m_comboBox; }
    QWidget *buttonWidget() const override { return m_manageButton; }
    void makeReadOnly() override { m_comboBox->setEnabled(false); }

    void refresh() override
    {
        m_model->setTypeFilter(DeviceTypeKitAspect::deviceTypeId(m_kit));
        m_comboBox->setCurrentIndex(m_model->indexOf(DeviceKitAspect::device(m_kit)));
    }

    void manageDevices()
    {
        Core::ICore::showOptionsDialog(Constants::DEVICE_SETTINGS_PAGE_ID, buttonWidget());
    }

    void modelAboutToReset()
    {
        m_selectedId = m_model->deviceId(m_comboBox->currentIndex());
        m_ignoreChange = true;
    }

    void modelReset()
    {
        m_comboBox->setCurrentIndex(m_model->indexForId(m_selectedId));
        m_ignoreChange = false;
    }

    void currentDeviceChanged()
    {
        if (m_ignoreChange)
            return;
        DeviceKitAspect::setDeviceId(m_kit, m_model->deviceId(m_comboBox->currentIndex()));
    }

    bool m_isReadOnly = false;
    bool m_ignoreChange = false;
    QComboBox *m_comboBox;
    QPushButton *m_manageButton;
    DeviceManagerModel *m_model;
    Core::Id m_selectedId;
};
} // namespace Internal

DeviceKitAspect::DeviceKitAspect()
{
    setObjectName(QLatin1String("DeviceInformation"));
    setId(DeviceKitAspect::id());
    setDisplayName(tr("Device"));
    setDescription(tr("The device to run the applications on."));
    setPriority(32000);

    connect(KitManager::instance(), &KitManager::kitsLoaded,
            this, &DeviceKitAspect::kitsWereLoaded);
}

QVariant DeviceKitAspect::defaultValue(const Kit *k) const
{
    Core::Id type = DeviceTypeKitAspect::deviceTypeId(k);
    // Use default device if that is compatible:
    IDevice::ConstPtr dev = DeviceManager::instance()->defaultDevice(type);
    if (dev && dev->isCompatibleWith(k))
        return dev->id().toString();
    // Use any other device that is compatible:
    for (int i = 0; i < DeviceManager::instance()->deviceCount(); ++i) {
        dev = DeviceManager::instance()->deviceAt(i);
        if (dev && dev->isCompatibleWith(k))
            return dev->id().toString();
    }
    // Fail: No device set up.
    return QString();
}

Tasks DeviceKitAspect::validate(const Kit *k) const
{
    IDevice::ConstPtr dev = DeviceKitAspect::device(k);
    Tasks result;
    if (dev.isNull())
        result.append(Task(Task::Warning, tr("No device set."),
                           Utils::FilePath(), -1, Core::Id(Constants::TASK_CATEGORY_BUILDSYSTEM)));
    else if (!dev->isCompatibleWith(k))
        result.append(Task(Task::Error, tr("Device is incompatible with this kit."),
                           Utils::FilePath(), -1, Core::Id(Constants::TASK_CATEGORY_BUILDSYSTEM)));

    return result;
}

void DeviceKitAspect::fix(Kit *k)
{
    IDevice::ConstPtr dev = DeviceKitAspect::device(k);
    if (!dev.isNull() && !dev->isCompatibleWith(k)) {
        qWarning("Device is no longer compatible with kit \"%s\", removing it.",
                 qPrintable(k->displayName()));
        setDeviceId(k, Core::Id());
    }
}

void DeviceKitAspect::setup(Kit *k)
{
    QTC_ASSERT(DeviceManager::instance()->isLoaded(), return);
    IDevice::ConstPtr dev = DeviceKitAspect::device(k);
    if (!dev.isNull() && dev->isCompatibleWith(k))
        return;

    setDeviceId(k, Core::Id::fromSetting(defaultValue(k)));
}

KitAspectWidget *DeviceKitAspect::createConfigWidget(Kit *k) const
{
    QTC_ASSERT(k, return nullptr);
    return new Internal::DeviceKitAspectWidget(k, this);
}

QString DeviceKitAspect::displayNamePostfix(const Kit *k) const
{
    IDevice::ConstPtr dev = device(k);
    return dev.isNull() ? QString() : dev->displayName();
}

KitAspect::ItemList DeviceKitAspect::toUserOutput(const Kit *k) const
{
    IDevice::ConstPtr dev = device(k);
    return {{tr("Device"), dev.isNull() ? tr("Unconfigured") : dev->displayName()}};
}

void DeviceKitAspect::addToMacroExpander(Kit *kit, Utils::MacroExpander *expander) const
{
    QTC_ASSERT(kit, return);
    expander->registerVariable("Device:HostAddress", tr("Host address"),
        [kit]() -> QString {
            const IDevice::ConstPtr device = DeviceKitAspect::device(kit);
            return device ? device->sshParameters().host() : QString();
    });
    expander->registerVariable("Device:SshPort", tr("SSH port"),
        [kit]() -> QString {
            const IDevice::ConstPtr device = DeviceKitAspect::device(kit);
            return device ? QString::number(device->sshParameters().port()) : QString();
    });
    expander->registerVariable("Device:UserName", tr("User name"),
        [kit]() -> QString {
            const IDevice::ConstPtr device = DeviceKitAspect::device(kit);
            return device ? device->sshParameters().userName() : QString();
    });
    expander->registerVariable("Device:KeyFile", tr("Private key file"),
        [kit]() -> QString {
            const IDevice::ConstPtr device = DeviceKitAspect::device(kit);
            return device ? device->sshParameters().privateKeyFile : QString();
    });
    expander->registerVariable("Device:Name", tr("Device name"),
        [kit]() -> QString {
            const IDevice::ConstPtr device = DeviceKitAspect::device(kit);
            return device ? device->displayName() : QString();
    });
}

Core::Id DeviceKitAspect::id()
{
    return "PE.Profile.Device";
}

IDevice::ConstPtr DeviceKitAspect::device(const Kit *k)
{
    QTC_ASSERT(DeviceManager::instance()->isLoaded(), return IDevice::ConstPtr());
    return DeviceManager::instance()->find(deviceId(k));
}

Core::Id DeviceKitAspect::deviceId(const Kit *k)
{
    return k ? Core::Id::fromSetting(k->value(DeviceKitAspect::id())) : Core::Id();
}

void DeviceKitAspect::setDevice(Kit *k, IDevice::ConstPtr dev)
{
    setDeviceId(k, dev ? dev->id() : Core::Id());
}

void DeviceKitAspect::setDeviceId(Kit *k, Core::Id id)
{
    QTC_ASSERT(k, return);
    k->setValue(DeviceKitAspect::id(), id.toSetting());
}

void DeviceKitAspect::kitsWereLoaded()
{
    foreach (Kit *k, KitManager::kits())
        fix(k);

    DeviceManager *dm = DeviceManager::instance();
    connect(dm, &DeviceManager::deviceListReplaced, this, &DeviceKitAspect::devicesChanged);
    connect(dm, &DeviceManager::deviceAdded, this, &DeviceKitAspect::devicesChanged);
    connect(dm, &DeviceManager::deviceRemoved, this, &DeviceKitAspect::devicesChanged);
    connect(dm, &DeviceManager::deviceUpdated, this, &DeviceKitAspect::deviceUpdated);

    connect(KitManager::instance(), &KitManager::kitUpdated,
            this, &DeviceKitAspect::kitUpdated);
    connect(KitManager::instance(), &KitManager::unmanagedKitUpdated,
            this, &DeviceKitAspect::kitUpdated);
}

void DeviceKitAspect::deviceUpdated(Core::Id id)
{
    foreach (Kit *k, KitManager::kits()) {
        if (deviceId(k) == id)
            notifyAboutUpdate(k);
    }
}

void DeviceKitAspect::kitUpdated(Kit *k)
{
    setup(k); // Set default device if necessary
}

void DeviceKitAspect::devicesChanged()
{
    foreach (Kit *k, KitManager::kits())
        setup(k); // Set default device if necessary
}

// --------------------------------------------------------------------------
// EnvironmentKitAspect:
// --------------------------------------------------------------------------
namespace Internal {
class EnvironmentKitAspectWidget : public KitAspectWidget
{
    Q_DECLARE_TR_FUNCTIONS(ProjectExplorer::EnvironmentKitAspect)

public:
    EnvironmentKitAspectWidget(Kit *workingCopy, const KitAspect *ki)
        : KitAspectWidget(workingCopy, ki),
          m_summaryLabel(new QLabel),
          m_manageButton(new QPushButton),
          m_mainWidget(new QWidget)
    {
        auto *layout = new QVBoxLayout;
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(m_summaryLabel);
        if (Utils::HostOsInfo::isWindowsHost())
            initMSVCOutputSwitch(layout);
        m_mainWidget->setLayout(layout);
        refresh();
        m_manageButton->setText(tr("Change..."));
        connect(m_manageButton, &QAbstractButton::clicked,
                this, &EnvironmentKitAspectWidget::editEnvironmentChanges);
    }

private:
    QWidget *mainWidget() const override { return m_mainWidget; }
    QWidget *buttonWidget() const override { return m_manageButton; }
    void makeReadOnly() override { m_manageButton->setEnabled(false); }

    void refresh() override
    {
        const QList<Utils::EnvironmentItem> changes = currentEnvironment();
        QString shortSummary = Utils::EnvironmentItem::toStringList(changes).join(QLatin1String("; "));
        QFontMetrics fm(m_summaryLabel->font());
        shortSummary = fm.elidedText(shortSummary, Qt::ElideRight, m_summaryLabel->width());
        m_summaryLabel->setText(shortSummary.isEmpty() ? tr("No changes to apply.") : shortSummary);
    }

    void editEnvironmentChanges()
    {
        bool ok;
        Utils::MacroExpander *expander = m_kit->macroExpander();
        Utils::EnvironmentDialog::Polisher polisher = [expander](QWidget *w) {
            Core::VariableChooser::addSupportForChildWidgets(w, expander);
        };
        QList<Utils::EnvironmentItem>
                changes = Utils::EnvironmentDialog::getEnvironmentItems(&ok,
                                                                        m_summaryLabel,
                                                                        currentEnvironment(),
                                                                        QString(),
                                                                        polisher);
        if (!ok)
            return;

        if (Utils::HostOsInfo::isWindowsHost()) {
            const Utils::EnvironmentItem forceMSVCEnglishItem("VSLANG", "1033");
            if (m_vslangCheckbox->isChecked() && changes.indexOf(forceMSVCEnglishItem) < 0)
                changes.append(forceMSVCEnglishItem);
        }

        EnvironmentKitAspect::setEnvironmentChanges(m_kit, changes);
    }

    QList<Utils::EnvironmentItem> currentEnvironment() const
    {
        QList<Utils::EnvironmentItem> changes = EnvironmentKitAspect::environmentChanges(m_kit);

        if (Utils::HostOsInfo::isWindowsHost()) {
            const Utils::EnvironmentItem forceMSVCEnglishItem("VSLANG", "1033");
            if (changes.indexOf(forceMSVCEnglishItem) >= 0) {
                m_vslangCheckbox->setCheckState(Qt::Checked);
                changes.removeAll(forceMSVCEnglishItem);
            }
        }

        Utils::sort(changes, [](const Utils::EnvironmentItem &lhs, const Utils::EnvironmentItem &rhs)
        { return QString::localeAwareCompare(lhs.name, rhs.name) < 0; });
        return changes;
    }

    void initMSVCOutputSwitch(QVBoxLayout *layout)
    {
        m_vslangCheckbox = new QCheckBox(tr("Force UTF-8 MSVC compiler output"));
        layout->addWidget(m_vslangCheckbox);
        m_vslangCheckbox->setToolTip(tr("Either switches MSVC to English or keeps the language and "
                                        "just forces UTF-8 output (may vary depending on the used MSVC "
                                        "compiler)."));
        connect(m_vslangCheckbox, &QCheckBox::toggled, this, [this](bool checked) {
            QList<Utils::EnvironmentItem> changes
                    = EnvironmentKitAspect::environmentChanges(m_kit);
            const Utils::EnvironmentItem forceMSVCEnglishItem("VSLANG", "1033");
            if (!checked && changes.indexOf(forceMSVCEnglishItem) >= 0)
                changes.removeAll(forceMSVCEnglishItem);
            if (checked && changes.indexOf(forceMSVCEnglishItem) < 0)
                changes.append(forceMSVCEnglishItem);
            EnvironmentKitAspect::setEnvironmentChanges(m_kit, changes);
        });
    }

    QLabel *m_summaryLabel;
    QPushButton *m_manageButton;
    QCheckBox *m_vslangCheckbox;
    QWidget *m_mainWidget;
};
} // namespace Internal

EnvironmentKitAspect::EnvironmentKitAspect()
{
    setObjectName(QLatin1String("EnvironmentKitAspect"));
    setId(EnvironmentKitAspect::id());
    setDisplayName(tr("Environment"));
    setDescription(tr("Additional build environment settings when using this kit."));
    setPriority(29000);
}

Tasks EnvironmentKitAspect::validate(const Kit *k) const
{
    Tasks result;
    QTC_ASSERT(k, return result);

    const QVariant variant = k->value(EnvironmentKitAspect::id());
    if (!variant.isNull() && !variant.canConvert(QVariant::List)) {
        result.append(Task(Task::Error, tr("The environment setting value is invalid."),
                           Utils::FilePath(), -1, Core::Id(Constants::TASK_CATEGORY_BUILDSYSTEM)));
    }
    return result;
}

void EnvironmentKitAspect::fix(Kit *k)
{
    QTC_ASSERT(k, return);

    const QVariant variant = k->value(EnvironmentKitAspect::id());
    if (!variant.isNull() && !variant.canConvert(QVariant::List)) {
        qWarning("Kit \"%s\" has a wrong environment value set.", qPrintable(k->displayName()));
        setEnvironmentChanges(k, QList<Utils::EnvironmentItem>());
    }
}

void EnvironmentKitAspect::addToEnvironment(const Kit *k, Utils::Environment &env) const
{
    const QStringList values
            = Utils::transform(Utils::EnvironmentItem::toStringList(environmentChanges(k)),
                               [k](const QString &v) { return k->macroExpander()->expand(v); });
    env.modify(Utils::EnvironmentItem::fromStringList(values));
}

KitAspectWidget *EnvironmentKitAspect::createConfigWidget(Kit *k) const
{
    QTC_ASSERT(k, return nullptr);
    return new Internal::EnvironmentKitAspectWidget(k, this);
}

KitAspect::ItemList EnvironmentKitAspect::toUserOutput(const Kit *k) const
{
    return { qMakePair(tr("Environment"),
             Utils::EnvironmentItem::toStringList(environmentChanges(k)).join("<br>")) };
}

Core::Id EnvironmentKitAspect::id()
{
    return "PE.Profile.Environment";
}

QList<Utils::EnvironmentItem> EnvironmentKitAspect::environmentChanges(const Kit *k)
{
     if (k)
         return Utils::EnvironmentItem::fromStringList(k->value(EnvironmentKitAspect::id()).toStringList());
     return QList<Utils::EnvironmentItem>();
}

void EnvironmentKitAspect::setEnvironmentChanges(Kit *k, const QList<Utils::EnvironmentItem> &changes)
{
    if (k)
        k->setValue(EnvironmentKitAspect::id(), Utils::EnvironmentItem::toStringList(changes));
}

} // namespace ProjectExplorer
