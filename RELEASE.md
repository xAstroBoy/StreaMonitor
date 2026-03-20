# StreaMonitor Release Guide

## Quick Release

To create and deploy a new release:

```bash
# Option 1: Auto-increment patch version and push tag
python release.py --push

# Option 2: Specify version and push tag
python release.py --version 2.2.0 --push

# Option 3: Create tag locally without pushing
python release.py --version 2.2.0
```

## Manual Build & Package

```bash
# Build locally
python build.py

# Build without deploying to runtime directory  
python build.py --no-deploy

# Package only (without building)
python build.py --package-only
```

## GitHub Actions Workflow

The CI/CD workflow automatically triggers when:

1. **Tag Push**: `git push origin v*` triggers the full build & release process
2. **Main Branch**: Builds and tests but doesn't create releases
3. **Pull Requests**: Builds and tests for validation

### Workflow Steps:
1. **Windows Build**: Uses vcpkg with static linking, creates portable .exe
2. **Linux Build**: Uses system libraries, creates .tar.gz
3. **Release**: Creates GitHub release with both platform binaries

## Release Process

### Automated (Recommended)
1. `python release.py --push` - Creates tag and triggers CI/CD
2. GitHub Actions builds for Windows/Linux
3. Artifacts are automatically attached to GitHub release

### Manual
1. `python build.py` - Build locally first
2. `python release.py --skip-build --push` - Tag and push without building
3. Monitor GitHub Actions for completion

## Version Management

Versions are automatically managed in [CMakeLists.txt](CMakeLists.txt):
- Current: Project line `VERSION X.Y.Z`
- Auto-increment: `--version-type patch/minor/major`
- Manual: `--version X.Y.Z`

## Troubleshooting

### Build Issues
- Ensure vcpkg is installed and `VCPKG_ROOT` is set
- Check dependencies in [vcpkg.json](vcpkg.json)
- Review build logs in `build_log.txt`

### Release Issues  
- Verify git working directory is clean
- Check GitHub repository permissions
- Ensure tag doesn't already exist

### GitHub Actions Issues
- Check workflow file: [.github/workflows/build.yml](.github/workflows/build.yml)
- Monitor build progress in GitHub Actions tab
- Verify artifacts are uploaded correctly

## Files Modified During Release

- **CMakeLists.txt**: Version number updated
- **Git tags**: New version tag created
- **dist/**: Local build artifacts (if built locally)

The GitHub Actions workflow handles all cross-platform building and packaging automatically.