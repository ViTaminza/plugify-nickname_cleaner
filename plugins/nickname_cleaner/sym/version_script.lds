{
    global:
        Plugify_*;

        /* s2sdk binding slots (defined by s2sdk_bindings, filled by the plugify module) */
        __s2sdk_*;

        /* pisexcompat resolves every displayed menu through hudmenu's exported
           GetClientMenuType (optional dependency); the plugin never displays a menu, but the
           library references the binding globals. */
        __hudmenu_*;

        /* No exported methods: nickname_cleaner exposes nothing to other plugins. */

    local: *;
};
