// SPDX-License-Identifier: GPL-2.0-or-later
import UIKit
import UniformTypeIdentifiers

/// Husk's own picker over Husk's Documents folder, for Android apps: files (ACTION_GET_CONTENT / OPEN_DOCUMENT) or a folder
/// (OPEN_DOCUMENT_TREE, which the system's picker cannot answer with a path the app can keep using). Folders open on a tap;
/// in folder mode "Use This Folder" answers with the folder shown. The answer is real paths inside Documents, so the app reads
/// and writes the files themselves, not copies.
@MainActor
enum HuskDocumentBrowser {
    enum Mode { case folder, files(types: [UTType], multiple: Bool) }

    private static var live: Session?

    /// `onPick` gets absolute paths; `onOther` (files mode only) is offered as "Other Locations…" for the system's picker.
    static func present(mode: Mode, onPick: @escaping ([String]) -> Void, onCancel: @escaping () -> Void,
                        onOther: (() -> Void)? = nil) {
        live?.cancel()
        let root = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].standardizedFileURL
        let session = Session(mode: mode, root: root, onPick: onPick, onCancel: onCancel, onOther: onOther)
        live = session
        let nav = UINavigationController(rootViewController: FolderController(session: session, folder: root))
        nav.presentationController?.delegate = session
        nav.modalPresentationStyle = .formSheet
        session.nav = nav
        guard let top = topController() else { live = nil; onCancel(); return }
        HuskLog.log("ui", "document browser: \(mode.isFolder ? "folder" : "files") from \(type(of: top))")
        top.present(nav, animated: true)
    }

    fileprivate static func clear(_ s: Session) { if live === s { live = nil } }

    private static func topController() -> UIViewController? {
        let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
        let window = scenes.flatMap { $0.windows }.first { $0.isKeyWindow } ?? scenes.flatMap { $0.windows }.first
        var top = window?.rootViewController
        while let next = top?.presentedViewController, !next.isBeingDismissed { top = next }
        return top
    }

    fileprivate final class Session: NSObject, UIAdaptivePresentationControllerDelegate {
        let mode: Mode
        let root: URL
        let onPick: ([String]) -> Void
        let onCancel: () -> Void
        let onOther: (() -> Void)?
        weak var nav: UINavigationController?
        var selected: [URL] = []
        private var done = false
        init(mode: Mode, root: URL, onPick: @escaping ([String]) -> Void, onCancel: @escaping () -> Void, onOther: (() -> Void)?) {
            self.mode = mode; self.root = root; self.onPick = onPick; self.onCancel = onCancel; self.onOther = onOther
        }
        func finish(_ urls: [URL]) {
            guard !done else { return }
            done = true
            HuskDocumentBrowser.clear(self)
            HuskLog.log("ui", "document browser: picked " + urls.map(\.lastPathComponent).joined(separator: ", "))
            let paths = urls.map(\.path), pick = onPick
            if let nav, nav.presentingViewController != nil { nav.dismiss(animated: true) { pick(paths) } } else { pick(paths) }
        }
        func cancel() {
            guard !done else { return }
            done = true
            HuskDocumentBrowser.clear(self)
            HuskLog.log("ui", "document browser: cancelled")
            let c = onCancel
            if let nav, nav.presentingViewController != nil { nav.dismiss(animated: true) { c() } } else { c() }
        }
        func other() {
            guard !done, let o = onOther else { return }
            done = true
            HuskDocumentBrowser.clear(self)
            if let nav, nav.presentingViewController != nil { nav.dismiss(animated: true) { o() } } else { o() }
        }
        // swiped down
        func presentationControllerDidDismiss(_ presentationController: UIPresentationController) {
            guard !done else { return }
            done = true
            HuskDocumentBrowser.clear(self)
            HuskLog.log("ui", "document browser: dismissed")
            onCancel()
        }
        func accepts(_ url: URL) -> Bool {
            guard case let .files(types, _) = mode else { return false }
            if types.contains(.item) || types.isEmpty { return true }
            guard let t = UTType(filenameExtension: url.pathExtension) else { return false }
            return types.contains { t.conforms(to: $0) }
        }
    }

    fileprivate final class FolderController: UITableViewController {
        let session: Session
        let folder: URL
        private var items: [(url: URL, dir: Bool, size: Int64)] = []

        init(session: Session, folder: URL) {
            self.session = session
            self.folder = folder
            super.init(style: .insetGrouped)
        }
        required init?(coder: NSCoder) { fatalError() }

        override func viewDidLoad() {
            super.viewDidLoad()
            title = folder.path == session.root.path ? "Husk Documents" : folder.lastPathComponent
            tableView.register(UITableViewCell.self, forCellReuseIdentifier: "c")
            if navigationController?.viewControllers.first === self {
                navigationItem.leftBarButtonItem = UIBarButtonItem(barButtonSystemItem: .cancel, target: self, action: #selector(cancelTapped))
            }
            let newFolder = UIBarButtonItem(image: UIImage(systemName: "folder.badge.plus"), style: .plain, target: self, action: #selector(newFolderTapped))
            switch session.mode {
            case .folder:
                navigationItem.rightBarButtonItem = newFolder
                navigationController?.isToolbarHidden = false
                toolbarItems = [UIBarButtonItem(systemItem: .flexibleSpace),
                                UIBarButtonItem(title: "Use This Folder", style: .done, target: self, action: #selector(useFolderTapped)),
                                UIBarButtonItem(systemItem: .flexibleSpace)]
            case let .files(_, multiple):
                var right: [UIBarButtonItem] = []
                if multiple { right.append(UIBarButtonItem(title: "Open", style: .done, target: self, action: #selector(openTapped))) }
                navigationItem.rightBarButtonItems = right
                if session.onOther != nil {
                    navigationController?.isToolbarHidden = false
                    toolbarItems = [UIBarButtonItem(title: "Other Locations…", style: .plain, target: self, action: #selector(otherTapped)),
                                    UIBarButtonItem(systemItem: .flexibleSpace), newFolder]
                }
            }
            reload()
        }

        override func viewWillAppear(_ animated: Bool) {
            super.viewWillAppear(animated)
            navigationController?.isToolbarHidden = toolbarItems?.isEmpty ?? true
            reload()
        }

        private func reload() {
            let keys: [URLResourceKey] = [.isDirectoryKey, .fileSizeKey]
            let urls = (try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: keys, options: [.skipsHiddenFiles])) ?? []
            let listed: [(url: URL, dir: Bool, size: Int64)] = urls.map { u in
                let v = try? u.resourceValues(forKeys: Set(keys))
                return (url: u.standardizedFileURL, dir: v?.isDirectory ?? false, size: Int64(v?.fileSize ?? 0))
            }
            items = listed.sorted { a, b in a.dir != b.dir ? a.dir : a.url.lastPathComponent.localizedStandardCompare(b.url.lastPathComponent) == .orderedAscending }
            tableView.reloadData()
            updateOpen()
        }

        private func updateOpen() {
            if case .files(_, true) = session.mode {
                let n = session.selected.count
                navigationItem.rightBarButtonItem?.isEnabled = n > 0
                navigationItem.rightBarButtonItem?.title = n > 0 ? "Open (\(n))" : "Open"
            }
        }

        override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int { max(items.count, 1) }

        override func tableView(_ tableView: UITableView, titleForFooterInSection section: Int) -> String? {
            if case .folder = session.mode { return "The app gets this folder and everything in it: \(relative(folder))" }
            return nil
        }

        override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
            let cell = tableView.dequeueReusableCell(withIdentifier: "c", for: indexPath)
            var conf = cell.defaultContentConfiguration()
            cell.accessoryType = .none
            cell.selectionStyle = .default
            if items.isEmpty {
                conf.text = "Empty folder"
                conf.textProperties.color = .secondaryLabel
                cell.selectionStyle = .none
                cell.contentConfiguration = conf
                return cell
            }
            let it = items[indexPath.row]
            conf.text = it.url.lastPathComponent
            if it.dir {
                conf.image = UIImage(systemName: "folder.fill")
                cell.accessoryType = .disclosureIndicator
            } else {
                conf.image = UIImage(systemName: "doc")
                conf.secondaryText = ByteCountFormatter.string(fromByteCount: it.size, countStyle: .file)
                let usable = session.accepts(it.url)
                if !usable {
                    conf.textProperties.color = .tertiaryLabel
                    conf.secondaryTextProperties.color = .tertiaryLabel
                    conf.imageProperties.tintColor = .tertiaryLabel
                    cell.selectionStyle = .none
                }
                if session.selected.contains(it.url) { cell.accessoryType = .checkmark }
            }
            cell.contentConfiguration = conf
            return cell
        }

        override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
            tableView.deselectRow(at: indexPath, animated: true)
            guard !items.isEmpty else { return }
            let it = items[indexPath.row]
            if it.dir {
                navigationController?.pushViewController(FolderController(session: session, folder: it.url), animated: true)
                return
            }
            guard session.accepts(it.url), case let .files(_, multiple) = session.mode else { return }
            if !multiple { session.finish([it.url]); return }
            if let k = session.selected.firstIndex(of: it.url) { session.selected.remove(at: k) } else { session.selected.append(it.url) }
            tableView.reloadRows(at: [indexPath], with: .none)
            updateOpen()
        }

        private func relative(_ u: URL) -> String {
            let r = session.root.path
            return u.path == r ? "Husk Documents" : "Husk Documents" + u.path.dropFirst(r.count)
        }

        @objc private func cancelTapped() { session.cancel() }
        @objc private func useFolderTapped() { session.finish([folder]) }
        @objc private func openTapped() { if !session.selected.isEmpty { session.finish(session.selected) } }
        @objc private func otherTapped() { session.other() }
        @objc private func newFolderTapped() {
            let a = UIAlertController(title: "New Folder", message: nil, preferredStyle: .alert)
            a.addTextField { $0.placeholder = "Name"; $0.autocapitalizationType = .sentences }
            a.addAction(UIAlertAction(title: "Cancel", style: .cancel))
            a.addAction(UIAlertAction(title: "Create", style: .default) { [weak self, weak a] _ in
                guard let self, let name = a?.textFields?.first?.text?.trimmingCharacters(in: .whitespaces), !name.isEmpty,
                      !name.contains("/") else { return }
                try? FileManager.default.createDirectory(at: self.folder.appendingPathComponent(name, isDirectory: true), withIntermediateDirectories: true)
                self.reload()
            })
            present(a, animated: true)
        }
    }
}

private extension HuskDocumentBrowser.Mode {
    var isFolder: Bool { if case .folder = self { return true }; return false }
}
