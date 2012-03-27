/*
    SSSD

    Authors:
        Stephen Gallagher <sgallagh@redhat.com>

    Copyright (C) 2012 Red Hat

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "util/util.h"
#include "providers/ad/ad_common.h"
#include "providers/ldap/ldap_common.h"
#include "providers/ldap/sdap_idmap.h"
#include "providers/krb5/krb5_auth.h"
#include "providers/ad/ad_id.h"

struct ad_options *ad_options = NULL;

static void
ad_shutdown(struct be_req *req);

struct bet_ops ad_id_ops = {
    .handler = ad_account_info_handler,
    .finalize = ad_shutdown,
    .check_online = sdap_check_online
};

struct bet_ops ad_auth_ops = {
    .handler = krb5_pam_handler,
    .finalize = NULL
};

struct bet_ops ad_chpass_ops = {
    .handler = krb5_pam_handler,
    .finalize = NULL
};

static errno_t
common_ad_init(struct be_ctx *bectx)
{
    errno_t ret;
    char *ad_servers = NULL;

    /* Get AD-specific options */
    ret = ad_get_common_options(bectx, bectx->cdb,
                                bectx->conf_path,
                                bectx->domain,
                                &ad_options);
    if (ret != EOK) {
        DEBUG(SSSDBG_FATAL_FAILURE,
              ("Could not parse common options: [%s]\n",
               strerror(ret)));
        goto done;
    }

    ad_servers = dp_opt_get_string(ad_options->basic, AD_SERVER);

    /* Set up the failover service */
    ret = ad_failover_init(ad_options, bectx, ad_servers, ad_options,
                           &ad_options->service);
    if (ret != EOK) {
        DEBUG(SSSDBG_FATAL_FAILURE,
              ("Failed to init AD failover service: [%s]\n",
               strerror(ret)));
        goto done;
    }

    ret = EOK;
done:
    return ret;
}

int
sssm_ad_id_init(struct be_ctx *bectx,
                struct bet_ops **ops,
                void **pvt_data)
{
    errno_t ret;
    struct ad_id_ctx *ad_ctx;
    struct sdap_id_ctx *sdap_ctx;

    if (!ad_options) {
        ret = common_ad_init(bectx);
        if (ret != EOK) {
            return ret;
        }
    }

    if (ad_options->id_ctx) {
        /* already initialized */
        *ops = &ad_id_ops;
        *pvt_data = ad_options->id_ctx;
        return EOK;
    }

    ad_ctx = talloc_zero(ad_options, struct ad_id_ctx);
    if (!ad_options) {
        return ENOMEM;
    }
    ad_ctx->ad_options = ad_options;
    ad_options->id_ctx = ad_ctx;

    sdap_ctx = talloc_zero(ad_options, struct sdap_id_ctx);
    if (!sdap_ctx) {
        return ENOMEM;
    }
    sdap_ctx->be = bectx;
    sdap_ctx->service = ad_options->service->sdap;
    ad_ctx->sdap_id_ctx = sdap_ctx;

    ret = ad_get_id_options(ad_options, bectx->cdb,
                            bectx->conf_path,
                            &sdap_ctx->opts);
    if (ret != EOK) {
        goto done;
    }

    ret = setup_tls_config(sdap_ctx->opts->basic);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE,
              ("setup_tls_config failed [%s]\n", strerror(ret)));
        goto done;
    }

    ret = sdap_id_conn_cache_create(sdap_ctx, sdap_ctx, &sdap_ctx->conn_cache);
    if (ret != EOK) {
        goto done;
    }

    if (dp_opt_get_bool(sdap_ctx->opts->basic, SDAP_ID_MAPPING)) {
        /* Set up the ID mapping object */
        ret = sdap_idmap_init(sdap_ctx, sdap_ctx, &sdap_ctx->opts->idmap_ctx);
        if (ret != EOK) goto done;
    }

    ret = sdap_id_setup_tasks(sdap_ctx);
    if (ret != EOK) {
        goto done;
    }

    ret = setup_child(sdap_ctx);
    if (ret != EOK) {
        DEBUG(SSSDBG_FATAL_FAILURE,
              ("setup_child failed [%d][%s].\n",
               ret, strerror(ret)));
        goto done;
    }

    *ops = &ad_id_ops;
    *pvt_data = ad_ctx;

    ret = EOK;
done:
    if (ret != EOK) {
        talloc_zfree(ad_options->id_ctx);
    }
    return ret;
}

static void
ad_shutdown(struct be_req *req)
{
    /* TODO: Clean up any internal data */
    sdap_handler_done(req, DP_ERR_OK, EOK, NULL);
}
