
#include <errno.h>
#include <yaml.h>

#include "telescope.h"

static int parse_option(telescope_global_t *glob, yaml_document_t *doc,
        yaml_node_t *key, yaml_node_t *value) {

    if (key->type == YAML_SCALAR_NODE && value->type == YAML_SCALAR_NODE
            && !strcmp((char *)key->data.scalar.value, "dagdev")) {
        glob->dagdev = strdup((char *)value->data.scalar.value);
    }

    else if (key->type == YAML_SCALAR_NODE && value->type == YAML_SCALAR_NODE
                 && !strcmp((char *)key->data.scalar.value, "monitorid")) {
        glob->monitorid = (uint16_t) strtoul((char *)value->data.scalar.value, NULL, 10);
    }

    else if (key->type == YAML_SCALAR_NODE && value->type == YAML_SCALAR_NODE
                 && !strcmp((char *)key->data.scalar.value, "mcastport")) {
        glob->mcastport = (uint16_t) strtoul((char *)value->data.scalar.value, NULL, 10);
    }

    else if (key->type == YAML_SCALAR_NODE && value->type == YAML_SCALAR_NODE
                 && !strcmp((char *)key->data.scalar.value, "mcastaddr")) {
        glob->mcastaddr = strdup((char *)value->data.scalar.value);
    }

    else if (key->type == YAML_SCALAR_NODE && value->type == YAML_SCALAR_NODE
                 && !strcmp((char *)key->data.scalar.value, "srcaddr")) {
        glob->srcaddr = strdup((char *)value->data.scalar.value);
    }

    else if (key->type == YAML_SCALAR_NODE && value->type == YAML_SCALAR_NODE
                 && !strcmp((char *)key->data.scalar.value, "mtu")) {
        glob->mtu = (uint16_t) strtoul((char *)value->data.scalar.value, NULL, 10);
    }

    else if (key->type == YAML_SCALAR_NODE && value->type == YAML_SCALAR_NODE
                 && !strcmp((char *)key->data.scalar.value, "filterfile")) {
        glob->filterfile= strdup((char *)value->data.scalar.value);
    }

    else if (key->type == YAML_SCALAR_NODE && value->type == YAML_SCALAR_NODE
                 && !strcmp((char *)key->data.scalar.value, "darknetoctet")) {
        glob->darknetoctet= atoi((char *)value->data.scalar.value);
    }

    else if (key->type == YAML_SCALAR_NODE && value->type == YAML_SCALAR_NODE
                 && !strcmp((char *)key->data.scalar.value, "statinterval")) {
        glob->statinterval = atoi((char *)value->data.scalar.value);
    }

    else if (key->type == YAML_SCALAR_NODE && value->type == YAML_SCALAR_NODE
                 && !strcmp((char *)key->data.scalar.value, "statdir")) {
        glob->statdir = strdup((char *)value->data.scalar.value);
    }

    return 1;
}

static int parse_yaml(telescope_global_t* glob,
                      char *configfile,
                      int (*parsefunc)(telescope_global_t* glob,
                          yaml_document_t *doc, yaml_node_t *,
                          yaml_node_t *)) {
    FILE *in = NULL;
    int ret = 0;

    /* YAML config parser. */
    yaml_parser_t parser;
    yaml_document_t document;
    yaml_node_t *root, *key, *value;
    yaml_node_pair_t *pair;

    if ((in = fopen(configfile, "r")) == NULL) {
        fprintf(stderr, "Failed to open config file: %s.\n", strerror(errno));
        return -1;
    }

    yaml_parser_initialize(&parser);
    yaml_parser_set_input_file(&parser, in);

    if (!yaml_parser_load(&parser, &document)) {
        fprintf(stderr, "Malformed config file.\n");
        ret = -1;
        goto yamlfail;
    }

    root = yaml_document_get_root_node(&document);
    if (!root) {
        fprintf(stderr, "Config file is empty!");
        ret = -1;
        goto endconfig;
    }

    if (root->type != YAML_MAPPING_NODE) {
        fprintf(stderr, "Top level of config should be a map.");
        ret = -1;
        goto endconfig;
    }

    /* Parse values */
    for (pair = root->data.mapping.pairs.start;
            pair < root->data.mapping.pairs.top;
                ++pair) {

        key = yaml_document_get_node(&document, pair->key);
        value = yaml_document_get_node(&document, pair->value);

        if ((ret = parsefunc(glob, &document, key, value)) <= 0) {
            break;
        }
        ret = 0;
    }

endconfig:
    yaml_document_delete(&document);
    yaml_parser_delete(&parser);

yamlfail:
    fclose(in);
    return ret;
}

telescope_global_t *telescope_init_global(char *configfile) {
    telescope_global_t *glob = NULL;

    if (configfile == NULL) {
        return NULL;
    }

    glob = (telescope_global_t *)malloc(sizeof(telescope_global_t));
    if (glob == NULL) {
        fprintf(stderr, "Failed to allocate memory for global variables\n");
        return NULL;
    }
    
    /* Initialization. */
    glob->dagdev = NULL;
    glob->mcastaddr = NULL;
    glob->srcaddr = NULL;
    glob->filterfile = NULL;
    glob->statdir = NULL;
    glob->monitorid = 1;
    glob->mcastport = 9001;
    glob->mtu = 1400;
    glob->darknetoctet = -1;
    glob->statinterval = 0;

    /* Parse config file. */
    if (parse_yaml(glob, configfile, parse_option) == -1) {
        telescope_cleanup_global(glob);    
        return NULL;
    }

    /* Try to set a sensible defaults. */
    if (glob->dagdev == NULL) {
        glob->dagdev = strdup("/dev/dag0");
    }

    if (glob->mcastaddr == NULL) {
        glob->mcastaddr = strdup("225.0.0.225");
    }

    if (glob->srcaddr == NULL) {
        fprintf(stderr,
            "Warning: no source address specified. Using default interface.\n");
        glob->srcaddr = strdup("0.0.0.0");
    }

    if (glob->monitorid == 0) {
        fprintf(stderr,
            "0 is not a valid monitor ID -- choose another number.\n");
        telescope_cleanup_global(glob);
        return NULL;
    }

    /* All done. */
    return glob;
}


void telescope_cleanup_global(telescope_global_t *glob) {
    if (glob == NULL) {
      return;
    }

    if (glob->dagdev) {
        free(glob->dagdev);
    }
    
    if (glob->mcastaddr) {
        free(glob->mcastaddr);
    }

    if (glob->srcaddr) {
        free(glob->srcaddr);
    }

    if (glob->statdir) {
        free(glob->statdir);
    }

    if (glob->filterfile) {
        free(glob->filterfile);
    }

    free(glob);
}

// vim: set sw=4 tabstop=4 softtabstop=4 expandtab :