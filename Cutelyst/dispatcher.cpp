/*
 * Copyright (C) 2013-2015 Daniel Nicoletti <dantti12@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB. If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "dispatcher_p.h"

#include "common.h"
#include "application.h"
#include "engine.h"
#include "context.h"
#include "controller.h"
#include "controller_p.h"
#include "action.h"
#include "request_p.h"
#include "dispatchtypepath.h"
#include "dispatchtypechained.h"
#include "utils.h"

#include <QUrl>
#include <QMetaMethod>
#include <QStringBuilder>

using namespace Cutelyst;

Dispatcher::Dispatcher(Application *app) : QObject(app)
  , d_ptr(new DispatcherPrivate(this))
{
    new DispatchTypePath(app);
    new DispatchTypeChained(app);
}

Dispatcher::~Dispatcher()
{
    delete d_ptr;
}

void Dispatcher::setupActions(const QList<Controller*> &controllers, const QList<Cutelyst::DispatchType *> &dispatchers)
{
    Q_D(Dispatcher);

    d->dispatchers = dispatchers;

    ActionList registeredActions;
    Q_FOREACH (Controller *controller, controllers) {
        bool instanceUsed = false;
        Q_FOREACH (Action *action, controller->actions()) {
            bool registered = false;
            if (!d->actions.contains(action->reverse())) {
                if (!action->attributes().contains(QStringLiteral("Private"))) {
                    // Register the action with each dispatcher
                    Q_FOREACH (DispatchType *dispatch, d->dispatchers) {
                        if (dispatch->registerAction(action)) {
                            registered = true;
                        }
                    }
                } else {
                    // We register private actions
                    registered = true;
                }
            }

            // The Begin, Auto, End actions are not
            // registered by Dispatchers but we need them
            // as private actions anyway
            if (registered) {
                d->actions.insert(action->ns() % QLatin1Char('/') % action->name(), action);
                d->actionContainer[action->ns()] << action;
                registeredActions.append(action);
                instanceUsed = true;
            } else if (action->name() != QLatin1String("_DISPATCH") &&
                       action->name() != QLatin1String("_BEGIN") &&
                       action->name() != QLatin1String("_AUTO") &&
                       action->name() != QLatin1String("_ACTION") &&
                       action->name() != QLatin1String("_END")) {
                qCDebug(CUTELYST_DISPATCHER) << "The action" << action->name() << "of"
                                             << action->controller()->objectName()
                                             << "controller was not registered in any dispatcher."
                                                " If you still want to access it internally (via actionFor())"
                                                " you may make it's method private.";
            } else if (d->showInternalActions) {
                qCCritical(CUTELYST_DISPATCHER) << "The action" << action->name() << "of"
                                                << action->controller()->objectName()
                                                << "controller was alread registered by the"
                                                << d->actions.value(action->reverse())->controller()->objectName()
                                                << "controller.";
                exit(1);
            }
        }

        if (instanceUsed) {
            d->controllers.insert(controller->objectName(), controller);
        }
    }

    // Cache root actions, BEFORE the controllers set them
    d->rootActions = d->actionContainer.value(QLatin1String(""));

    Q_FOREACH (Controller *controller, controllers) {
        controller->d_ptr->setupFinished();
    }

    // Unregister any dispatcher that is not in use
    int i = 0;
    while (i < d->dispatchers.size()) {
        DispatchType *type = d->dispatchers.at(i);
        if (!type->inUse()) {
            d->dispatchers.removeAt(i);
            continue;
        }
        ++i;
    }

    if (qobject_cast<Application*>(parent())->engine()->workerCore() == 0) {
        d->printActions();
    }
}

bool Dispatcher::dispatch(Context *c)
{
    Action *action = c->action();
    if (action) {
        Controller *controller = action->controller();
        if (controller) {
            return controller->_DISPATCH(c);
        }
        return forward(c, QLatin1Char('/') % action->ns() % QLatin1String("/_DISPATCH"));
    } else {
        const QString &path = c->req()->path();
        if (path.isEmpty()) {
            c->error(tr("No default action defined"));
        } else {
            c->error(tr("Unknown resource \"%1\".").arg(path));
        }
    }
    return false;
}

bool Dispatcher::forward(Context *c, Component *component)
{
    Q_ASSERT(component);
    // If the component was an Action
    // the dispatch() would call c->execute
    return c->execute(component);
}

bool Dispatcher::forward(Context *c, const QString &opname)
{
    Q_D(const Dispatcher);

    Action *action = d->command2Action(c, opname, c->request()->args());
    if (action) {
        return action->dispatch(c);
    }

    qCCritical(CUTELYST_DISPATCHER) << "Action not found" << opname << c->request()->args();
    return false;
}

void Dispatcher::prepareAction(Context *c)
{
    Q_D(Dispatcher);

    Request *request = c->request();
    const QString &path = request->path();
    QVector<QStringRef> pathParts = path.splitRef(QLatin1Char('/'));
    QStringList args;

    int pos = path.size();

    //  "foo/bar"
    //  "foo/" skip
    //  "foo"
    //  ""
    Q_FOREVER {
        // Check out the dispatch types to see if any
        // will handle the path at this level
        const QString &actionPath = path.mid(0, pos);
        Q_FOREACH (DispatchType *type, d->dispatchers) {
            if (type->match(c, actionPath, args) == DispatchType::ExactMatch) {
                goto out;
            }
        }

        // leave the loop if we are at the root "/"
        if (pos <= 0) {
            break;
        }

        pos = path.lastIndexOf(QLatin1Char('/'), --pos);
        if (pos == -1) {
            pos = 0;
        }

        // If not, move the last part path to args
        if (pathParts.isEmpty()) {
            args.prepend(QString());
        } else {
            args.prepend(QUrl::fromPercentEncoding(pathParts.takeLast().toLatin1()));
        }
    }

out:
    if (!request->match().isEmpty()) {
        qCDebug(CUTELYST_DISPATCHER) << "Path is" << request->match();
    }

    if (!request->args().isEmpty()) {
        qCDebug(CUTELYST_DISPATCHER) << "Arguments are" << request->args().join(QLatin1Char('/'));
    }
}

Action *Dispatcher::getAction(const QString &name, const QString &nameSpace) const
{
    Q_D(const Dispatcher);

    if (name.isEmpty()) {
        return 0;
    }

    if (nameSpace.isEmpty()) {
        return d->actions.value(QLatin1Char('/') % name);
    }

    const QString ns = DispatcherPrivate::cleanNamespace(nameSpace);
    return d->actions.value(ns % QLatin1Char('/') % name);
}

Action *Dispatcher::getActionByPath(const QString &path) const
{
    Q_D(const Dispatcher);

    QString _path = path;
    if (_path.startsWith(QLatin1Char('/'))) {
        _path.remove(0, 1);
    }
    return d->actions.value(_path);
}

ActionList Dispatcher::getActions(const QString &name, const QString &nameSpace) const
{
    Q_D(const Dispatcher);

    if (name.isEmpty()) {
        return ActionList();
    }

    const QString &ns = DispatcherPrivate::cleanNamespace(nameSpace);

    ActionList ret;
    const ActionList &containers = d->getContainers(ns);
    Q_FOREACH (Action *action, containers) {
        if (action->name() == name) {
            ret.prepend(action);
        }
    }
    return ret;
}

QMap<QString, Controller *> Dispatcher::controllers() const
{
    Q_D(const Dispatcher);
    return d->controllers;
}

QString Dispatcher::uriForAction(Action *action, const QStringList &captures) const
{
    Q_D(const Dispatcher);
    Q_FOREACH (DispatchType *dispatch, d->dispatchers) {
        QString uri = dispatch->uriForAction(action, captures);
        if (!uri.isNull()) {
            return uri.isEmpty() ? QStringLiteral("/") : uri;
        }
    }
    return QString();
}

Action *Dispatcher::expandAction(Context *c, Action *action) const
{
    Q_D(const Dispatcher);
    Q_FOREACH (DispatchType *dispatch, d->dispatchers) {
        Action *expandedAction = dispatch->expandAction(c, action);
        if (expandedAction) {
            return expandedAction;
        }
    }
    return action;
}

QList<DispatchType *> Dispatcher::dispatchers() const
{
    Q_D(const Dispatcher);
    return d->dispatchers;
}

QString DispatcherPrivate::cleanNamespace(const QString &ns)
{
    QString ret = ns;
    bool lastWasSlash = true; // remove initial slash
    int nsSize = ns.size();
    for (int i = 0; i < nsSize; ++i) {
        // Mark if the last char was a slash
        // so that two or more consecutive slashes
        // could be converted to just one
        // "a///b" -> "a/b"
        if (ret.at(i) == QLatin1Char('/')) {
            if (lastWasSlash) {
                ret.remove(i, 1);
                --nsSize;
            } else {
                lastWasSlash = true;
            }
        } else {
            lastWasSlash = false;
        }
    }
    return ret;
}

DispatcherPrivate::DispatcherPrivate(Dispatcher *q) : q_ptr(q)
{
}

void DispatcherPrivate::printActions() const
{
    QList<QStringList> table;

    QStringList keys = actions.keys();
    keys.sort(Qt::CaseInsensitive);
    Q_FOREACH (const QString &key, keys) {
        Action *action = actions.value(key);
        if (showInternalActions || !action->name().startsWith(QLatin1Char('_'))) {
            QString path = key;
            if (!path.startsWith(QLatin1Char('/'))) {
                path.prepend(QLatin1Char('/'));
            }

            QStringList row;
            row.append(path);
            row.append(action->className());
            row.append(action->name());
            table.append(row);
        }
    }

    qCDebug(CUTELYST_DISPATCHER) <<  Utils::buildTable(table, {
                                                           QStringLiteral("Private"),
                                                           QStringLiteral("Class"),
                                                           QStringLiteral("Method")
                                                       },
                                                       QStringLiteral("Loaded Private actions:")).constData();

    // List all public actions
    Q_FOREACH (DispatchType *dispatch, dispatchers) {
        qCDebug(CUTELYST_DISPATCHER) << dispatch->list().constData();
    }
}

ActionList DispatcherPrivate::getContainers(const QString &ns) const
{
    ActionList ret;

    if (ns != QLatin1String("/")) {
        int pos = ns.size();
//        qDebug() << pos << ns.mid(0, pos);
        while (pos > 0) {
//            qDebug() << pos << ns.mid(0, pos);
            ret.append(actionContainer.value(ns.mid(0, pos)));
            pos = ns.lastIndexOf(QLatin1Char('/'), pos - 1);
        }
    }
//    qDebug() << actionContainer.size() << rootActions;
    ret.append(rootActions);

    return ret;
}

Action *DispatcherPrivate::command2Action(Context *c, const QString &command, const QStringList &args) const
{
    auto it = actions.constFind(command);
    if (it != actions.constEnd()) {
        return it.value();
    }

    return invokeAsPath(c, command, args);
}

Action *DispatcherPrivate::invokeAsPath(Context *c, const QString &relativePath, const QStringList &args) const
{
    Q_Q(const Dispatcher);

    Action *ret;
    QString path = DispatcherPrivate::actionRel2Abs(c, relativePath);

    int pos = path.lastIndexOf(QLatin1Char('/'));
    int lastPos = path.size();
    do {
        if (pos == -1) {
            ret = q->getAction(path, QString());
            if (ret) {
                return ret;
            }
        } else {
            const QString name = path.mid(pos + 1, lastPos);
            path = path.mid(0, pos);
            ret = q->getAction(name, path);
            if (ret) {
                return ret;
            }
        }

        lastPos = pos;
        pos = path.indexOf(QLatin1Char('/'), pos - 1);
    } while (pos != -1);

    return 0;
}

QString DispatcherPrivate::actionRel2Abs(Context *c, const QString &path)
{
    if (path.startsWith(QLatin1Char('/'))) {
        return path.mid(1);
    }

    const QString ns = qobject_cast<Action *>(c->stack().last())->ns();
    if (ns.isEmpty()) {
        return path;
    }
    return ns % QLatin1Char('/') % path;
}
