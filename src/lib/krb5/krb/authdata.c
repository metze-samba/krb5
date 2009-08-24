/*
 * Copyright 2009 by the Massachusetts Institute of Technology.  All
 * Rights Reserved.
 *
 * Export of this software from the United States of America may
 *   require a specific license from the United States Government.
 *   It is the responsibility of any person or organization contemplating
 *   export to obtain such a license before exporting.
 *
 * WITHIN THAT CONSTRAINT, permission to use, copy, modify, and
 * distribute this software and its documentation for any purpose and
 * without fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation, and that
 * the name of M.I.T. not be used in advertising or publicity pertaining
 * to distribution of the software without specific, written prior
 * permission.  Furthermore if you modify this software you must label
 * your software as modified software and not distribute it in such a
 * fashion that it might be confused with the original M.I.T. software.
 * M.I.T. makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is" without express
 * or implied warranty.
 *
 */

#include "k5-int.h"
#include "authdata.h"
#include "auth_con.h"

/* Based on preauth2.c */

#define DEBUG 1

#if TARGET_OS_MAC
static const char *objdirs[] = { KRB5_AUTHDATA_PLUGIN_BUNDLE_DIR, LIBDIR "/krb5/plugins/authdata", NULL }; /* should be a list */
#else
static const char *objdirs[] = { LIBDIR "/krb5/plugins/authdata", NULL };
#endif

static krb5plugin_authdata_client_ftable_v0 *authdata_systems[] = { &krb5int_mspac_authdata_client_ftable, NULL };

static inline int
count_ad_modules(krb5plugin_authdata_client_ftable_v0 *table)
{
    int i;

    if (table->ad_type_list == NULL)
        return 0;

    for (i = 0; table->ad_type_list[i]; i++)
        ;

    return i;
}

static krb5_error_code
init_ad_system(krb5_context kcontext, krb5_authdata_context context,
               krb5plugin_authdata_client_ftable_v0 *table,
               int *module_count)
{
    int j, k = *module_count;
    krb5_error_code code;
    void *plugin_context = NULL;
    void **rcpp;

    if (table->ad_type_list == NULL) {
#ifdef DEBUG
        fprintf(stderr, "warning: module \"%s\" does not advertise any AD types\n", table->name);
#endif
        return ENOENT;
    }
    if (table->init == NULL)
        return ENOSYS;

    code = (*table->init)(kcontext, &plugin_context);
    if (code != 0) {
#ifdef DEBUG
        fprintf(stderr, "warning: skipping module \"%s\" which failed to initialize\n", table->name);
#endif
        return code;
    }

    for (j = 0; table->ad_type_list[j] != 0; j++) {
        context->modules[k].ad_type = table->ad_type_list[j];
        context->modules[k].plugin_context = plugin_context;
        if (j == 0)
            context->modules[k].client_fini = table->fini;
        else
            context->modules[k].client_fini = NULL;
        context->modules[k].ftable = table;
        context->modules[k].name = table->name;
        if (table->flags != NULL) {
            (*table->flags)(kcontext, plugin_context,
                            context->modules[k].ad_type, &context->modules[k].flags);
        } else {
            context->modules[k].flags = 0;
        }
        context->modules[k].request_context = NULL;
        if (j == 0) {
            context->modules[k].client_req_init = table->request_init;
            context->modules[k].client_req_fini = table->request_fini;
            rcpp = &context->modules[k].request_context;
        } else {
            context->modules[k].client_req_init = NULL;
            context->modules[k].client_req_fini = NULL;
        }
        context->modules[k].request_context_pp = rcpp;

#ifdef DEBUG
        fprintf(stderr, "init module \"%s\", ad_type %d, flags %08x\n",
                context->modules[k].name,
                context->modules[k].ad_type,
                context->modules[k].flags);
#endif
        k++;
    }
    *module_count = k;

    return 0;
}

krb5_error_code
krb5int_authdata_context_init(krb5_context kcontext, krb5_authdata_context *pcontext)
{
    int n_modules, n_tables, i, k;
    void **tables = NULL;
    krb5plugin_authdata_client_ftable_v0 *table;
    krb5_authdata_context context = NULL;
    int internal_count = 0;
    struct plugin_dir_handle plugins;

    *pcontext = NULL;
    memset(&plugins, 0, sizeof(plugins));

    n_modules = 0;
    for (n_tables = 0; authdata_systems[n_tables] != NULL; n_tables++) {
        n_modules += count_ad_modules(authdata_systems[n_tables]);
    }
    internal_count = n_tables;

    if (PLUGIN_DIR_OPEN(&plugins) == 0 &&
        krb5int_open_plugin_dirs(objdirs, NULL,
                                 &plugins,
                                 &kcontext->err) == 0 &&
        krb5int_get_plugin_dir_data(&plugins,
                                    "authdata_client_0",
                                    &tables,
                                    &kcontext->err) == 0 &&
        tables != NULL)
    {
        for (; tables[n_tables - internal_count] != NULL; n_tables++) {
            table = tables[n_tables - internal_count];
            n_modules += count_ad_modules(table);
        }
    }

    context = (krb5_authdata_context)calloc(1, sizeof(*context));
    if (kcontext == NULL) {
        if (tables != NULL)
            krb5int_free_plugin_dir_data(tables);
        krb5int_close_plugin_dirs(&context->plugins);
        return ENOMEM;
    }
    context->modules = (struct _krb5_authdata_context_module *)calloc(n_modules, sizeof(context->modules[0]));
    if (context->modules == NULL) {
        if (tables != NULL)
            krb5int_free_plugin_dir_data(tables);
        krb5int_close_plugin_dirs(&context->plugins);
        free(kcontext);
        return ENOMEM;
    }
    context->n_modules = n_modules;

    /* fill in the structure */
    k = 0;

    for (i = 0; i < n_tables - internal_count; i++) {
        (void) init_ad_system(kcontext, context, tables[i], &k);
    }
    for (i = 0; i < internal_count; i++) {
        (void) init_ad_system(kcontext, context, authdata_systems[i], &k);
    }

    if (tables != NULL)
        krb5int_free_plugin_dir_data(tables);

    context->plugins = plugins;

    *pcontext = context;

    return 0;
}

void KRB5_CALLCONV
krb5_authdata_context_free(krb5_context kcontext, krb5_authdata_context context)
{
    int i;

    if (context == NULL)
        return;

    for (i = 0; i < context->n_modules; i++) {
        struct _krb5_authdata_context_module *module = &context->modules[i];

        if (module->client_req_fini != NULL && module->request_context != NULL)
            (*module->client_req_fini)(kcontext, module->plugin_context, module->request_context);

        if (module->client_fini != NULL)
            (*module->client_fini)(kcontext, module->plugin_context);

        memset(module, 0, sizeof(*module));
    }

    if (context->modules != NULL) {
        free(context->modules);
        context->modules = NULL;
    }
    krb5int_close_plugin_dirs(&context->plugins);
    memset(context, 0, sizeof(*context));
    free(context);
}

krb5_error_code KRB5_CALLCONV
krb5_authdata_request_context_init(krb5_context kcontext,
                                   krb5_authdata_context context,
                                   krb5_flags usage)
{
    int i;
    krb5_error_code code;

#if 0
    if (context == NULL)
        context = kcontext->authdata_context;
    if (context == NULL) {
        code = krb5int_authdata_context_init(kcontext, &context);
        if (code != 0)
            return code;

        kcontext->authdata_context = context;
    }
#endif

    for (i = 0; i < context->n_modules; i++) {
        struct _krb5_authdata_context_module *module = &context->modules[i];

        if (module->client_req_init == NULL)
            continue;

        if ((module->flags & usage) == 0)
            continue;

        code = (*module->client_req_init)(kcontext,
                                          module->plugin_context,
                                          usage,
                                          module->request_context_pp);
        if ((code != 0 && code != ENOMEM) &&
            (module->flags & AD_INFORMATIONAL))
            code = 0;
        if (code != 0)
            break;
    }

    return code;
}

void KRB5_CALLCONV
krb5_authdata_request_context_fini(krb5_context kcontext,
                                   krb5_authdata_context context)
{
    int i;

#if 0
    if (context == NULL)
        context = kcontext->authdata_context;
#endif
    if (context == NULL)
        return;

    for (i = 0; i < context->n_modules; i++) {
        struct _krb5_authdata_context_module *module = &context->modules[i];

        if (module->client_req_fini == NULL)
            continue;

        if (module->request_context == NULL)
            continue;

        (*module->client_req_fini)(kcontext, module->plugin_context, module->request_context);
        module->request_context = NULL;
    }
}

krb5_error_code
krb5int_verify_authdata(krb5_context kcontext,
                        krb5_authdata_context context,
                        const krb5_auth_context *auth_context,
                        const krb5_keyblock *key,
                        const krb5_ap_req *ap_req,
                        krb5_flags flags)
{
    int i;
    krb5_error_code code;

    for (i = 0; i < context->n_modules; i++) {
        struct _krb5_authdata_context_module *module = &context->modules[i];
        krb5_authdata **authdata;

        if (module->ftable->request_verify == NULL)
            continue;

        code = krb5int_find_authdata(kcontext,
                                     ap_req->ticket->enc_part2->authorization_data,
                                     (*auth_context)->authentp->authorization_data,
                                     module->ad_type,
                                     &authdata);
        if (code != 0 || authdata == NULL)
            continue;

        assert(authdata[0] != NULL);

        code = (*module->ftable->request_verify)(kcontext,
                                                 module->plugin_context,
                                                 *(module->request_context_pp),
                                                 auth_context,
                                                 key,
                                                 ap_req,
                                                 flags,
                                                 authdata);
        if (code != 0 && (module->flags & AD_INFORMATIONAL))
            code = 0;
        krb5_free_authdata(kcontext, authdata);
        if (code != 0)
            break;
    }

    return code;
}

static krb5_error_code
merge_data_array_nocopy(krb5_data **dst, krb5_data *src, unsigned int *len)
{
    unsigned int i;

    if (src == NULL)
        return 0;

    for (i = 0; src[i].data != NULL; i++)
        ;

    *dst = (krb5_data *)realloc(*dst, (*len + i + 1) * sizeof(krb5_data));
    if (*dst == NULL)
        return ENOMEM;

    memcpy(&(*dst)[*len], src, i * sizeof(krb5_data));

    *len += i;

    (*dst)[*len].data = NULL;

    return 0;
}

krb5_error_code KRB5_CALLCONV
krb5_authdata_get_attribute_types(krb5_context kcontext,
                                  krb5_authdata_context context,
                                  krb5_data **asserted_attrs,
                                  krb5_data **verified_attrs)
{
    int i;
    krb5_error_code code;
    krb5_data *asserted = NULL;
    krb5_data *verified = NULL;
    unsigned int len = 0;

    for (i = 0; i < context->n_modules; i++) {
        struct _krb5_authdata_context_module *module = &context->modules[i];
        krb5_data *asserted2 = NULL;
        krb5_data *verified2 = NULL;

        if (module->ftable->get_attribute_types == NULL)
            continue;

        if ((*module->ftable->get_attribute_types)(kcontext,
                                                   module->plugin_context,
                                                   module->request_context,
                                                   &asserted2,
                                                   &verified2) != 0)
            continue;

        code = merge_data_array_nocopy(&asserted, asserted2, &len);
        if (code != 0)
            break;

        code = merge_data_array_nocopy(&verified, verified2, &len);
        if (code != 0)
            break;

        if (asserted2 != NULL)
            free(asserted2);
        if (verified2 != NULL)
            free(verified2);
    }

    if (code == 0) {
        *asserted_attrs = asserted;
        *verified_attrs = verified;
    }

    return code;
}

krb5_error_code KRB5_CALLCONV
krb5_authdata_get_attribute(krb5_context kcontext,
                            krb5_authdata_context context,
                            const krb5_data *attribute,
                            krb5_boolean *authenticated,
                            krb5_boolean *complete,
                            krb5_data *value,
                            krb5_data *display_value,
                            int *more)
{
    int i;
    krb5_error_code code = ENOENT;

    /* NB at present a plugin is presumed to be authoritative for an attribute */
    for (i = 0; i < context->n_modules; i++) {
        struct _krb5_authdata_context_module *module = &context->modules[i];

        if (module->ftable->get_attribute == NULL)
            continue;

        code = (*module->ftable->get_attribute)(kcontext,
                                                module->plugin_context,
                                                module->request_context,
                                                attribute,
                                                authenticated,
                                                complete,
                                                value,
                                                display_value,
                                                more);
        if (code == 0)
            break;
    }

    return code;
}

krb5_error_code KRB5_CALLCONV
krb5_authdata_set_attribute(krb5_context kcontext,
                            krb5_authdata_context context,
                            krb5_boolean complete,
                            const krb5_data *attribute,
                            const krb5_data *value)
{
    int i;
    krb5_error_code code = ENOENT;

    for (i = 0; i < context->n_modules; i++) {
        struct _krb5_authdata_context_module *module = &context->modules[i];

        if (module->ftable->set_attribute == NULL)
            continue;

        code = (*module->ftable->set_attribute)(kcontext,
                                                module->plugin_context,
                                                module->request_context,
                                                complete,
                                                attribute,
                                                value);
        if (code != 0 && code != ENOENT)
            break;
    }

    return code;

}

krb5_error_code KRB5_CALLCONV
krb5_authdata_delete_attribute(krb5_context kcontext,
                               krb5_authdata_context context,
                               const krb5_data *attribute)
{
    int i;
    krb5_error_code code = ENOENT;

    for (i = 0; i < context->n_modules; i++) {
        struct _krb5_authdata_context_module *module = &context->modules[i];

        if (module->ftable->set_attribute == NULL)
            continue;

        code = (*module->ftable->delete_attribute)(kcontext,
                                                   module->plugin_context,
                                                   module->request_context,
                                                   attribute);
        if (code != 0 && code != ENOENT)
            break;
    }

    return code;
}

krb5_error_code KRB5_CALLCONV
krb5_authdata_export_attributes(krb5_context kcontext,
                                krb5_authdata_context context,
                                krb5_authdata ***pauthdata)
{
    int i;
    krb5_error_code code = ENOENT;
    krb5_authdata **authdata = NULL;
    unsigned int len = 0;

    *pauthdata = NULL;

    for (i = 0; i < context->n_modules; i++) {
        struct _krb5_authdata_context_module *module = &context->modules[i];
        krb5_authdata **authdata2 = NULL;
        int j;

        if (module->ftable->export_attributes == NULL)
            continue;

        code = (*module->ftable->export_attributes)(kcontext,
                                                    module->plugin_context,
                                                    module->request_context,
                                                    &authdata2);
        if (code != 0 && code != ENOENT)
            break;

        if (authdata2 == NULL)
            continue;

        for (j = 0; authdata2[j] != NULL; j++)
            ;

        authdata = (krb5_authdata **)realloc(authdata, (len + j + 1) * sizeof(krb5_authdata *));
        if (authdata == NULL)
            return ENOMEM;

        memcpy(&authdata[len], authdata2, j * sizeof(krb5_authdata *));

        len += j;
    }

    *pauthdata = authdata;

    return code;
}

krb5_error_code KRB5_CALLCONV
krb5_authdata_export_internal(krb5_context kcontext,
                              krb5_authdata_context context,
                              krb5_authdatatype type,
                              void **ptr)
{
    int i;
    krb5_error_code code = ENOENT;

    *ptr = NULL;

    for (i = 0; i < context->n_modules; i++) {
        struct _krb5_authdata_context_module *module = &context->modules[i];

        if (module->ad_type != type)
            continue;

        if (module->ftable->export_internal == NULL)
            continue;

        code = (*module->ftable->export_internal)(kcontext,
                                                  module->plugin_context,
                                                  module->request_context,
                                                  ptr);

        break;
    }

    return code;
}

krb5_error_code KRB5_CALLCONV
krb5_authdata_free_internal(krb5_context kcontext,
                            krb5_authdata_context context,
                            krb5_authdatatype type,
                            void *ptr)
{
    int i;
    krb5_error_code code = ENOENT;

    for (i = 0; i < context->n_modules; i++) {
        struct _krb5_authdata_context_module *module = &context->modules[i];

        if (module->ad_type != type)
            continue;

        if (module->ftable->free_internal == NULL)
            continue;

        (*module->ftable->free_internal)(kcontext,
                                         module->plugin_context,
                                         module->request_context,
                                         ptr);

        break;
    }

    return code;
}

#ifdef DEBUG
static void
debug_authdata_attribute(krb5_context kcontext,
                         krb5_authdata_context context,
                         const krb5_data *attr)
{
    krb5_error_code code;
    krb5_boolean authenticated, complete;
    krb5_data value, display_value;
    int more = -1;

    while (more != 0) {
        code = krb5_authdata_get_attribute(kcontext, context, attr,
                                           &authenticated, &complete,
                                           &value, &display_value, &more);
        if (code != 0)
            break;

        fprintf(stderr, "AD Attribute %.*s Value Length %d Disp Value Length %d More %d\n",
                attr->length, attr->data, value.length, display_value.length, more);

        krb5_free_data_contents(kcontext, &value);
        krb5_free_data_contents(kcontext, &display_value);
    }
}

void
krb5_authdata_debug(krb5_context kcontext,
                    krb5_authdata_context context)
{
    krb5_error_code code;
    krb5_data *asserted = NULL;
    krb5_data *verified = NULL;
    int i;

#if 0
    {
    krb5_data fooattr = { KV5M_DATA, sizeof("mspac:1234"), "mspac:1234" };
    krb5_data foovalue = { KV5M_DATA, sizeof("abcdefghijklmnop"), "abcdefghijklmnop" };

    code = krb5_authdata_set_attribute(kcontext, context, TRUE, &fooattr, &foovalue);
    if (code != 0) {
        fprintf(stderr, "krb5_authdata_debug failed: %s\n",
                krb5_get_error_message(kcontext, code));
    }
    }
#endif

    code = krb5_authdata_get_attribute_types(kcontext, context,
                                             &asserted, &verified);
    if (code != 0) {
        fprintf(stderr, "krb5_authdata_debug failed: %s\n",
                krb5_get_error_message(kcontext, code));
        return;
    }

    fprintf(stderr, "Asserted attributes:\n");
    if (asserted != NULL) {
        for (i = 0; asserted[i].data != NULL; i++) {
            debug_authdata_attribute(kcontext, context, &asserted[i]);
        }
    }
    fprintf(stderr, "Authenticated attributes:\n");
    if (verified != NULL) {
        for (i = 0; verified[i].data != NULL; i++) {
            debug_authdata_attribute(kcontext, context, &verified[i]);
        }
    }
    krb5int_free_data_list(kcontext, asserted);
    krb5int_free_data_list(kcontext, verified);
}
#endif /* DEBUG */

