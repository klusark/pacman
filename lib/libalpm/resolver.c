#include "alpm_list.h"
#include "package.h"
#include "deps.h"
#include "resolver.h"
#include "db.h"
#include "handle.h"

#include <string.h>

struct _alpm_resolver_pkg {
	alpm_pkg_t *pkg;
	alpm_list_t *rdeps;
	alpm_list_t *owners;
	int disabled;
	int picked;
};

struct _alpm_resolver_dep {
	struct _alpm_resolver_pkg *rpkg;
	alpm_depend_t *dep;
	alpm_list_t *satisfiers;
};

struct _alpm_resolver_conflict {
	struct _alpm_resolver_pkg *rpkg1;
	struct _alpm_resolver_pkg *rpkg2;
};

enum _alpm_resolver_action_type {
	_ALPM_RESOLVER_ACTION_INSTALL = 1,
	_ALPM_RESOLVER_ACTION_UNINSTALL,
};

struct _alpm_resolver_action {
	enum _alpm_resolver_action_type type;
	alpm_pkg_t *target;
	alpm_list_t *causing_pkgs;
};

typedef struct _alpm_resolver_pkg rpkg_t;
typedef struct _alpm_resolver_dep rdep_t;
typedef struct _alpm_resolver_conflict rconflict_t;

static alpm_list_t *_alpm_resolver_satisfiers(alpm_depend_t *dep, alpm_list_t *pool, int flags)
{
	alpm_list_t *i, *satisfiers = NULL;
	alpm_depmod_t mod = dep->mod;
	if(flags & ALPM_RESOLVER_IGNORE_DEPENDENCY_VERSION) {
		dep->mod = ALPM_DEP_MOD_ANY;
	}
	for(i = pool; i; i = i->next) {
		if(_alpm_depcmp(i->data, dep)) {
			alpm_list_append(&satisfiers, i->data);
		}
	}
	dep->mod = mod;
	return satisfiers;
}

#define PKGORIGIN(x) (x)->origin == ALPM_PKG_FROM_LOCALDB ? "local" : "sync"

static struct _alpm_resolver_pkg *_alpm_resolver_extend_graph(
		alpm_handle_t *handle, alpm_list_t **graph, alpm_pkg_t *pkg, alpm_list_t *pool, int flags)
{
	alpm_list_t *i;
	for(i = *graph; i; i = i->next) {
		rpkg_t *rpkg = i->data;
		if(pkg == rpkg->pkg) {
			return rpkg;
		}
	}
	printf("extending graph with %s/%s\n", pkg->origin == ALPM_PKG_FROM_LOCALDB ? "local" : "sync", pkg->name);

	alpm_list_t *j;
	struct _alpm_resolver_pkg *rpkg = malloc(sizeof(struct _alpm_resolver_pkg));
	alpm_list_append(graph, rpkg);

	rpkg->pkg = pkg;
	rpkg->owners = NULL;
	rpkg->rdeps = NULL;
	rpkg->disabled = 0;
	rpkg->picked = 0;

	for(j = alpm_pkg_get_depends(pkg); j; j = j->next) {
		struct _alpm_resolver_dep *rdep;
		if(_alpm_depcmp_provides(j->data, handle->assumeinstalled)) {
			continue;
		}

		rdep = malloc(sizeof(struct _alpm_resolver_dep));
		alpm_list_t *satisfiers = _alpm_resolver_satisfiers(j->data, pool, flags);
		alpm_list_append(&(rpkg->rdeps), rdep);
		rdep->satisfiers = NULL;
		rdep->dep = j->data;
		rdep->rpkg = rpkg;
		printf("resolving %s/%s dep %s\n", PKGORIGIN(pkg), pkg->name, rdep->dep->name);
		if(satisfiers == NULL) {
			printf("no satisfiers found for %s %s\n", pkg->name, rdep->dep->name);
			return NULL;
		}
		for(i = satisfiers; i; i = i->next) {
			rpkg_t *satisfier = _alpm_resolver_extend_graph(handle, graph, i->data, pool, flags);
			if(satisfier == NULL) {
				alpm_list_free(satisfiers);
				puts("extend graph failed");
				return NULL;
			}
			alpm_list_append(&(rdep->satisfiers), satisfier);
			alpm_list_append(&(satisfier->owners), rdep);
		}
		alpm_list_free(satisfiers);
	}
	return rpkg;
}

static void _alpm_resolver_reduce(rpkg_t *rpkg, alpm_list_t **solution)
{
	alpm_list_t *i, *j;
	if(rpkg->disabled || rpkg->picked) {
		return;
	}
	rpkg->picked = 1;
	printf("reducing %s\n", rpkg->pkg->name);
	if(rpkg->pkg->origin != ALPM_PKG_FROM_LOCALDB) {
		printf("appending %s\n", rpkg->pkg->name);
		alpm_list_append(solution, rpkg->pkg);
	}
	for(i = rpkg->rdeps; i; i = i->next) {
		rdep_t *rdep = i->data;
		for(j = rdep->satisfiers; j; j = j->next) {
			rpkg_t *rsatisfier = j->data;
			if(!rsatisfier->disabled) {
				_alpm_resolver_reduce(rsatisfier, solution);
				break;
			}
		}
	}
}

static int _alpm_resolver_solve_conflicts(alpm_list_t *conflicts, alpm_list_t *roots)
{
	/* grab the first conflict */
	rconflict_t *conflict;
	int pkg1_can_be_disabled = 1;
	int pkg2_can_be_disabled = 1;
	alpm_list_t *i;

	if(conflicts == NULL) {
		return 0;
	}

	conflict = conflicts->data;

	/* check if conflict has already been resolved */
	if(conflict->rpkg1->disabled || conflict->rpkg2->disabled) {
		return _alpm_resolver_solve_conflicts(conflicts->next, roots);
	}

	printf("resolving %s/%s - %s/%s\n",
			PKGORIGIN(conflict->rpkg1->pkg), conflict->rpkg1->pkg->name,
			PKGORIGIN(conflict->rpkg2->pkg), conflict->rpkg2->pkg->name);

	/* rpkg1 is the preferred package, try disabling rpkg2 first */
	/* check if rpkg2 can be disabled */
	for(i = roots; i && pkg2_can_be_disabled; i = i->next) {
		if(conflict->rpkg2 == i->data && conflict->rpkg2->pkg->origin != ALPM_PKG_FROM_LOCALDB) {
			printf("%s/%s cannot be disabled because it is a root\n",
					PKGORIGIN(conflict->rpkg2->pkg), conflict->rpkg2->pkg->name);
			pkg2_can_be_disabled = 0;
		}
	}
	for(i = conflict->rpkg2->owners; i && pkg2_can_be_disabled; i = i->next) {
			rdep_t *rdep = i->data;
			int dep_has_alt_satisfier = 0;
			alpm_list_t *k;
			for(k = rdep->satisfiers; k && !dep_has_alt_satisfier; k = k->next) {
				rpkg_t *satisfier = k->data;
				if(satisfier != conflict->rpkg2 && !satisfier->disabled) {
					dep_has_alt_satisfier = 1;
				}
			}
			if(!dep_has_alt_satisfier) {
				printf("%s/%s cannot be disabled because it is a sole dependency of %s/%s\n",
						PKGORIGIN(conflict->rpkg2->pkg), conflict->rpkg2->pkg->name,
						PKGORIGIN(rdep->rpkg->pkg), rdep->rpkg->pkg->name);
				pkg2_can_be_disabled = 0;
			}
	}
	if(pkg2_can_be_disabled) {
		conflict->rpkg2->disabled = 1;
		if(_alpm_resolver_solve_conflicts(conflicts->next, roots) == 0) {
			printf("disabling %s/%s\n",
					PKGORIGIN(conflict->rpkg2->pkg), conflict->rpkg2->pkg->name);
			return 0;
		}
		conflict->rpkg2->disabled = 0;
	}

	/* check if rpkg1 can be disabled */
	for(i = roots; i && pkg1_can_be_disabled; i = i->next) {
		if(conflict->rpkg1 == i->data && conflict->rpkg1->pkg->origin != ALPM_PKG_FROM_LOCALDB) {
			pkg1_can_be_disabled = 0;
			printf("%s/%s cannot be disabled because it is a root\n",
					PKGORIGIN(conflict->rpkg1->pkg), conflict->rpkg1->pkg->name);
		}
	}
	for(i = conflict->rpkg1->owners; i && pkg1_can_be_disabled; i = i->next) {
			rdep_t *rdep = i->data;
			int dep_has_alt_satisfier = 0;
			alpm_list_t *k;
			for(k = rdep->satisfiers; k && !dep_has_alt_satisfier; k = k->next) {
				rpkg_t *satisfier = k->data;
				printf("satisfier - %s/%s - %s\n",
						PKGORIGIN(satisfier->pkg), satisfier->pkg->name, satisfier->disabled ? "disabled" : "enabled");
				if(satisfier != conflict->rpkg1 && !satisfier->disabled) {
					dep_has_alt_satisfier = 1;
				}
			}
			if(!dep_has_alt_satisfier) {
				printf("%s/%s cannot be disabled because it is a sole dependency of %s/%s\n",
						PKGORIGIN(conflict->rpkg1->pkg), conflict->rpkg1->pkg->name,
						PKGORIGIN(rdep->rpkg->pkg), rdep->rpkg->pkg->name);
				pkg1_can_be_disabled = 0;
			}
	}
	if(pkg1_can_be_disabled) {
		conflict->rpkg1->disabled = 1;
		if(_alpm_resolver_solve_conflicts(conflicts->next, roots) == 0) {
			printf("disabling %s/%s\n",
					PKGORIGIN(conflict->rpkg1->pkg), conflict->rpkg1->pkg->name);
			return 0;
		}
		conflict->rpkg1->disabled = 0;
	}

	printf("unable to resolve conflict between %s - %s\n",
			conflict->rpkg1->pkg->name, conflict->rpkg2->pkg->name);
	return -1;
}

static int _alpm_resolver_pkgs_conflict(alpm_pkg_t *pkg1, alpm_pkg_t *pkg2) {
	alpm_list_t *i;
	if(strcmp(pkg1->name, pkg2->name) == 0) {
		printf("%s conflicts with %s\n", pkg1->name, pkg2->name);
		return 1;
	}
	for(i = alpm_pkg_get_conflicts(pkg1); i; i = i->next) {
		alpm_depend_t *conflict = i->data;
		if(_alpm_depcmp(pkg2, conflict)) {
		printf("%s conflicts with %s\n", pkg1->name, pkg2->name);
			return 1;
		}
	}
	for(i = alpm_pkg_get_conflicts(pkg2); i; i = i->next) {
		alpm_depend_t *conflict = i->data;
		if(_alpm_depcmp(pkg1, conflict)) {
		printf("%s conflicts with %s\n", pkg1->name, pkg2->name);
			return 1;
		}
	}
	return 0;
}

static alpm_list_t *_alpm_resolver_find_conflicts(alpm_list_t *graph)
{
	alpm_list_t *i, *j, *conflicts = NULL;
	for(i = graph; i; i = i->next) {
		rpkg_t *rpkg1 = i->data;
		for(j = i->next; j; j = j->next) {
			rpkg_t *rpkg2 = j->data;
			printf("checking conflict %s %s\n", rpkg1->pkg->name, rpkg2->pkg->name);
			if(_alpm_resolver_pkgs_conflict(rpkg1->pkg, rpkg2->pkg)) {
				rconflict_t *rconflict = malloc(sizeof(rconflict_t));
				rconflict->rpkg1 = rpkg1;
				rconflict->rpkg2 = rpkg2;
				alpm_list_append(&conflicts, rconflict);
			}
		}
	}
	return conflicts;
}

static alpm_list_t *_alpm_resolver_solve(alpm_list_t *graph, alpm_list_t *roots)
{
	alpm_list_t *i;
	alpm_list_t *conflicts = _alpm_resolver_find_conflicts(graph);
	alpm_list_t *solution = NULL;
	if(_alpm_resolver_solve_conflicts(conflicts, roots) != 0) {
		puts("solve conflicts failed");
		FREELIST(conflicts);
		return NULL;
	}
	for(i = roots; i; i = i->next) {
		_alpm_resolver_reduce(i->data, &solution);
	}
	FREELIST(conflicts);
	return solution;
}

alpm_list_t *_alpm_resolvedeps_thorough(alpm_handle_t *handle, alpm_list_t *add, alpm_list_t *remove, int flags)
{
	alpm_list_t *i, *graph = NULL, *roots = NULL;
	alpm_list_t *pool = NULL;
	alpm_list_t *solution = NULL;

	puts("resolvedeps_thorough");

	for(i = add; i; i = i->next) {
		alpm_pkg_t *pkg = i->data;
		printf("appending add/%s to pool\n", pkg->name);
		alpm_list_append(&pool, pkg);
	}
	for(i = _alpm_db_get_pkgcache(handle->db_local); i; i = i->next) {
		alpm_pkg_t *pkg = i->data;
		if(!alpm_pkg_find(add, pkg->name) && !alpm_pkg_find(remove, pkg->name)) {
			printf("appending local/%s to pool\n", pkg->name);
			alpm_list_append(&pool, pkg);
		}
	}
	for(i = handle->dbs_sync; i; i = i->next) {
		alpm_list_t *j;
		for(j = _alpm_db_get_pkgcache(i->data); j; j = j->next) {
			alpm_pkg_t *pkg = j->data;
			if(!alpm_pkg_find(add, pkg->name) && !alpm_pkg_find(remove, pkg->name) && !alpm_pkg_should_ignore(handle, pkg)) {
				printf("appending sync/%s to pool\n", pkg->name);
				alpm_list_append(&pool, pkg);
			}
		}
	}

	for(i = add; i; i = i->next) {
		/* seed the graph with packages we know we need */
		rpkg_t *rpkg = _alpm_resolver_extend_graph(handle, &graph, i->data, pool, flags);
		if(rpkg == NULL) {
			puts("extend graph failed");
			goto cleanup;
		}
		alpm_list_append(&roots, rpkg);
	}
	for(i = _alpm_db_get_pkgcache(handle->db_local); i; i = i->next) {
		/* seed the pool with currently installed packages to make sure we don't
		 * break their dependencies */
		alpm_pkg_t *pkg = i->data;
		if(!alpm_pkg_find(add, pkg->name) && !alpm_pkg_find(remove, pkg->name)) {
			rpkg_t *rpkg = _alpm_resolver_extend_graph(handle, &graph, pkg, pool, flags);
			if(rpkg == NULL) {
				puts("extend graph failed");
				goto cleanup;
			}
			alpm_list_append(&roots, rpkg);
		}
	}
	solution = _alpm_resolver_solve(graph, roots);
	printf("solution: %zd graph: %zd roots: %zd\n", alpm_list_count(solution), alpm_list_count(graph), alpm_list_count(roots));
	puts("resolvedeps_cleanup");

cleanup:
	for(i = graph; i; i = i->next) {
		rpkg_t *rpkg = i->data;
		alpm_list_t *j;
		for(j = rpkg->rdeps; j; j = j->next) {
			rdep_t *rdep = j->data;
			alpm_list_free(rdep->satisfiers);
			free(rdep);
		}
		alpm_list_free(rpkg->rdeps);
		alpm_list_free(rpkg->owners);
		free(rpkg);
	}
	alpm_list_free(pool);
	alpm_list_free(graph);
	alpm_list_free(roots);

	puts(solution ? "found solution" : "no solution found");

	return solution;
}

/*
 input: list of packages to add/remove
 graph - all packages to install and all local packages
 pool - all packages available to satisfy dependencies,
	 all packages being installed, sync packages, and local packages not being removed
 roots - packages whose dependencies must be satisfied,
   packages being installed and local packages
*/