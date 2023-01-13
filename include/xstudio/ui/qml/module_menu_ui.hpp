// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <caf/all.hpp>
#include <caf/io/all.hpp>

CAF_PUSH_WARNINGS
#include <QAbstractListModel>
#include <QUrl>
#include <QAbstractItemModel>
#include <QQmlPropertyMap>
CAF_POP_WARNINGS

#include "xstudio/ui/qml/helper_ui.hpp"
#include "xstudio/module/module.hpp"

/**
 *  @brief ModuleMenusModel class.
 *
 *  @details
 *   This class defines a custom type in QML called 'XsModuleMenu'. The type can be used to
 *   expose in QML any backend 'attributes' belonging to a Module that include data for the
 *   'MenuPath' role. The MenuPath role data is one or more strings containing sub strings
 *   delimitied  by a | (pipe) symbol, and these strings define the 'path' to the menu item
 *   that will expose the attribute state in the UI.
 *
 *   For example, let's say you have a multi choice attribute in your module created as follows:
 *
          channel_ = add_string_choice_attribute(
              "Channel",
              "Chan",
              "RGB",
              {"RGB","Red","Green","Blue","Alpha","Luminance"},
              {"RGB","R","G","B","A","L"}
          );
          channel_->set_role_data(
              module::Attribute::MenuPaths,
              std::vector<std::string>({"my_dynamic_menu|Colour|Channel"})
          );
 *
 *   This means we can add a 'Colour' sub-menu to any QML Menu in the front end, with a
 'Channel'
 *   menu under the 'Colour' menu and then under the 'Channel' menu there will be 5 checkable
 *   menu items labelled 'Red', 'Green', 'Blue', 'Alpha', 'Luminance' and from there the user
 *   can set the value of your 'channel_' property in the back end Module.
 *
 *   To create the Menu in QML you will need this:
 *
      Menu {

        id: myMenu
        title: "My Module Menu"

        XsModuleMenuBuilder {
          parent_menu: myMenu
          root_menu_name: "my_dynamic_menu"
        }
      }
 *
 *
 *   Note that menus are built dymically, so runtime changes to the 'MenuPaths' role data on
 *   an attribute will be reflected in the UI
 */

namespace xstudio {
namespace ui {
    namespace qml {

        class ModuleAttrsToQMLShim;

        class ModuleMenusModel : public QAbstractListModel {

            Q_OBJECT

          public:
            enum XsMenuRoles {
                MenuText = Qt::UserRole + 1024,
                IsCheckable,
                IsChecked,
                IsMultiChoice,
                Value,
                Enabled,
                IsDivider,
                Uuid,
                AttrType
            };

            inline static const std::map<int, std::string> role_names = {
                {MenuText, "xs_module_menu_item_text"},
                {IsCheckable, "xs_module_menu_item_checkable"},
                {IsChecked, "xs_module_menu_item_checked"},
                {IsMultiChoice, "xs_module_menu_item_is_multichoice"},
                {Value, "xs_module_menu_item_value"},
                {Enabled, "xs_module_menu_item_enabled"},
                {IsDivider, "xs_module_menu_item_is_divider"},
                {Uuid, "xs_module_menu_item_uuid"},
                {AttrType, "xs_module_menu_item_attr_type"}};

            Q_PROPERTY(int num_submenus READ num_submenus NOTIFY num_submenusChanged)
            Q_PROPERTY(QString root_menu_name READ rootMenuName WRITE setRootMenuName NOTIFY
                           rootMenuNameChanged)
            Q_PROPERTY(QString title READ title NOTIFY titleChanged)
            Q_PROPERTY(QStringList submenu_names READ submenu_names NOTIFY submenu_namesChanged)

            ModuleMenusModel(QObject *parent = nullptr);

            ~ModuleMenusModel() override;

            [[nodiscard]] int rowCount(const QModelIndex &parent) const override;

            [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;

            [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

            bool setData(const QModelIndex &index, const QVariant &value, int role) override;

            void add_attributes_from_backend(const module::AttributeSet &attrs);

            [[nodiscard]] Qt::ItemFlags flags(const QModelIndex &) const override {
                return Qt::ItemIsEnabled | Qt::ItemIsEditable;
            }

            void update_attribute_from_backend(
                const utility::Uuid &attr_uuid,
                const int role,
                const utility::JsonStore &role_value);

            void update_full_attribute_from_backend(const module::ConstAttributePtr &attr);

            void remove_attribute(const utility::Uuid &attr_uuid);

            [[nodiscard]] int num_submenus() const { return submenu_names_.size(); }

          signals:

            void setAttributeFromFrontEnd(const QUuid, const int, const QVariant);
            void rootMenuNameChanged(QString);
            void num_submenusChanged();
            void titleChanged();
            void submenu_namesChanged();

          public slots:

            [[nodiscard]] QString rootMenuName() const { return menu_path_; }
            [[nodiscard]] QString title() const { return title_; }
            void setRootMenuName(QString p);
            // QObject * submenu(int index);
            [[nodiscard]] QStringList submenu_names() const { return submenu_names_; }

          private:
            bool already_have_attr_in_this_menu(const QUuid &uuid);
            bool is_attr_in_this_menu(const module::ConstAttributePtr &attr);
            void add_multi_choice_menu_item(const module::ConstAttributePtr &attr);
            void add_checkable_menu_item(const module::ConstAttributePtr &attr);
            void add_menu_action_item(const module::ConstAttributePtr &attr);
            void update_multi_choice_menu_item(
                const utility::Uuid &attr_uuid, const utility::JsonStore &string_choice_data);

            std::vector<QMap<int, QVariant>> attributes_data_;

            QStringList submenu_names_;
            QMap<QString, QList<QUuid>> attrs_per_submenus_;

            // QList<ModuleMenusModel *> submenus_;
            QString menu_path_;
            QString title_;
            int menu_nesting_depth_     = {0};
            ModuleAttrsToQMLShim *shim_ = {nullptr};
        };

    } // namespace qml
} // namespace ui
} // namespace xstudio