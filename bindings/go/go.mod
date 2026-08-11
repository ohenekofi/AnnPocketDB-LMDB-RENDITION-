module github.com/annpocketdb/annpocketdb-go

go 1.21

// No external Go dependencies.
// The only external dependency is libannpocketdb_c (C shared library),
// linked at build time via cgo directives in annpocketdb.go.
